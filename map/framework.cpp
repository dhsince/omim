#include "map/framework.hpp"

#include "map/dialog_settings.hpp"
#include "map/ge0_parser.hpp"
#include "map/geourl_process.hpp"
#include "map/storage_bridge.hpp"

#include "defines.hpp"

#include "routing/online_absent_fetcher.hpp"
#include "routing/osrm_router.hpp"
#include "routing/road_graph_router.hpp"
#include "routing/route.hpp"
#include "routing/routing_algorithm.hpp"

#include "search/search_engine.hpp"
#include "search/search_query_factory.hpp"
#include "search/result.hpp"

#include "drape_frontend/gui/country_status_helper.hpp"

#include "drape_frontend/visual_params.hpp"

#include "indexer/categories_holder.hpp"
#include "indexer/classificator_loader.hpp"
#include "indexer/drawing_rules.hpp"
#include "indexer/feature.hpp"
#include "indexer/map_style_reader.hpp"
#include "indexer/mwm_version.hpp"
#include "indexer/scales.hpp"

/// @todo Probably it's better to join this functionality.
//@{
#include "indexer/feature_algo.hpp"
#include "indexer/feature_utils.hpp"
//@}

#include "platform/local_country_file_utils.hpp"
#include "platform/measurement_utils.hpp"
#include "platform/platform.hpp"
#include "platform/preferred_languages.hpp"
#include "platform/settings.hpp"

#include "coding/internal/file_data.hpp"
#include "coding/zip_reader.hpp"
#include "coding/url_encode.hpp"
#include "coding/file_name_utils.hpp"
#include "coding/png_memory_encoder.hpp"

#include "geometry/angles.hpp"
#include "geometry/distance_on_sphere.hpp"

#include "base/math.hpp"
#include "base/timer.hpp"
#include "base/scope_guard.hpp"

#include "std/algorithm.hpp"
#include "std/bind.hpp"
#include "std/target_os.hpp"
#include "std/vector.hpp"

#include "api/internal/c/api-client-internals.h"
#include "api/src/c/api-client.h"

#include "3party/Alohalytics/src/alohalytics.h"

#define KMZ_EXTENSION ".kmz"

#define DEFAULT_BOOKMARK_TYPE "placemark-red"

using namespace storage;
using namespace routing;
using namespace location;

using platform::CountryFile;
using platform::LocalCountryFile;

#ifdef FIXED_LOCATION
Framework::FixedPosition::FixedPosition()
{
  m_fixedLatLon = Settings::Get("FixPosition", m_latlon);
  m_fixedDir = Settings::Get("FixDirection", m_dirFromNorth);
}
#endif

namespace
{
  static const int BM_TOUCH_PIXEL_INCREASE = 20;
}

pair<MwmSet::MwmHandle, MwmSet::RegResult> Framework::RegisterMap(
    LocalCountryFile const & localFile)
{
  string const countryFileName = localFile.GetCountryName();
  LOG(LINFO, ("Loading map:", countryFileName));
  return m_model.RegisterMap(localFile);
}

void Framework::OnLocationError(TLocationError /*error*/)
{
  CallDrapeFunction(bind(&df::DrapeEngine::CancelMyPosition, _1));
}

void Framework::OnLocationUpdate(GpsInfo const & info)
{
#ifdef FIXED_LOCATION
  GpsInfo rInfo(info);

  // get fixed coordinates
  m_fixedPos.GetLon(rInfo.m_longitude);
  m_fixedPos.GetLat(rInfo.m_latitude);

  // pretend like GPS position
  rInfo.m_horizontalAccuracy = 5.0;

  if (m_fixedPos.HasNorth())
  {
    // pass compass value (for devices without compass)
    CompassInfo compass;
    m_fixedPos.GetNorth(compass.m_bearing);
    OnCompassUpdate(compass);
  }

#else
  GpsInfo rInfo(info);
#endif
  location::RouteMatchingInfo routeMatchingInfo;
  CheckLocationForRouting(rInfo);

  MatchLocationToRoute(rInfo, routeMatchingInfo);

  CallDrapeFunction(bind(&df::DrapeEngine::SetGpsInfo, _1, rInfo,
                         m_routingSession.IsNavigable(), routeMatchingInfo));
}

void Framework::OnCompassUpdate(CompassInfo const & info)
{
#ifdef FIXED_LOCATION
  CompassInfo rInfo(info);
  m_fixedPos.GetNorth(rInfo.m_bearing);
#else
  CompassInfo const & rInfo = info;
#endif

  CallDrapeFunction(bind(&df::DrapeEngine::SetCompassInfo, _1, rInfo));
}

void Framework::SwitchMyPositionNextMode()
{
  CallDrapeFunction(bind(&df::DrapeEngine::MyPositionNextMode, _1));
}

void Framework::InvalidateMyPosition()
{
  ASSERT(m_drapeEngine != nullptr, ());
  CallDrapeFunction(bind(&df::DrapeEngine::InvalidateMyPosition, _1));
}

void Framework::SetMyPositionModeListener(location::TMyPositionModeChanged const & fn)
{
  ASSERT(m_drapeEngine != nullptr, ());
  CallDrapeFunction(bind(&df::DrapeEngine::SetMyPositionModeListener, _1, fn));
}

void Framework::OnUserPositionChanged(m2::PointD const & position)
{
  MyPositionMarkPoint * myPostition = UserMarkContainer::UserMarkForMyPostion();
  myPostition->SetPtOrg(position);

  if (IsRoutingActive())
    m_routingSession.SetUserCurrentPosition(position);
}

void Framework::CallDrapeFunction(TDrapeFunction const & fn)
{
  if (m_drapeEngine)
    fn(m_drapeEngine.get());
}

void Framework::StopLocationFollow()
{
  CallDrapeFunction(bind(&df::DrapeEngine::StopLocationFollow, _1));
}

Framework::Framework()
  : m_bmManager(*this)
  , m_fixedSearchResults(0)
{
  m_activeMaps.reset(new ActiveMapsLayout(*this));
  m_globalCntTree = storage::CountryTree(m_activeMaps);
  m_storageBridge = make_unique_dp<StorageBridge>(m_activeMaps, bind(&Framework::UpdateCountryInfo, this, _1, false));
  // Init strings bundle.
  // @TODO. There are hardcoded strings below which are defined in strings.txt as well.
  // It's better to use strings form strings.txt intead of hardcoding them here.
  m_stringsBundle.SetDefaultString("country_status_added_to_queue", "^\nis added to the downloading queue");
  m_stringsBundle.SetDefaultString("country_status_downloading", "Downloading\n^\n^");
  m_stringsBundle.SetDefaultString("country_status_download", "Download map\n^ ^");
  m_stringsBundle.SetDefaultString("country_status_download_routing", "Download Map + Routing\n^ ^");
  m_stringsBundle.SetDefaultString("country_status_download_failed", "Downloading\n^\nhas failed");
  m_stringsBundle.SetDefaultString("try_again", "Try Again");
  m_stringsBundle.SetDefaultString("not_enough_free_space_on_sdcard", "Not enough space for downloading");

  m_stringsBundle.SetDefaultString("dropped_pin", "Dropped Pin");
  m_stringsBundle.SetDefaultString("my_places", "My Places");
  m_stringsBundle.SetDefaultString("my_position", "My Position");
  m_stringsBundle.SetDefaultString("routes", "Routes");

  m_stringsBundle.SetDefaultString("routing_failed_unknown_my_position", "Current location is undefined. Please specify location to create route.");
  m_stringsBundle.SetDefaultString("routing_failed_has_no_routing_file", "Additional data is required to create the route. Download data now?");
  m_stringsBundle.SetDefaultString("routing_failed_start_point_not_found", "Cannot calculate the route. No roads near your starting point.");
  m_stringsBundle.SetDefaultString("routing_failed_dst_point_not_found", "Cannot calculate the route. No roads near your destination.");
  m_stringsBundle.SetDefaultString("routing_failed_cross_mwm_building", "Routes can only be created that are fully contained within a single map.");
  m_stringsBundle.SetDefaultString("routing_failed_route_not_found", "There is no route found between the selected origin and destination.Please select a different start or end point.");
  m_stringsBundle.SetDefaultString("routing_failed_internal_error", "Internal error occurred. Please try to delete and download the map again. If problem persist please contact us at support@maps.me.");

#ifdef DRAW_TOUCH_POINTS
  m_informationDisplay.enableDebugPoints(true);
#endif

  m_model.InitClassificator();
  m_model.SetOnMapDeregisteredCallback(bind(&Framework::OnMapDeregistered, this, _1));
  LOG(LDEBUG, ("Classificator initialized"));

  // To avoid possible races - init search engine once in constructor.
  (void)GetSearchEngine();
  LOG(LDEBUG, ("Search engine initialized"));

#ifndef OMIM_OS_ANDROID
  RegisterAllMaps();
#endif
  LOG(LDEBUG, ("Maps initialized"));

  // Init storage with needed callback.
  m_storage.Init(bind(&Framework::UpdateAfterDownload, this, _1));
  LOG(LDEBUG, ("Storage initialized"));

#ifdef USE_PEDESTRIAN_ROUTER
  SetRouter(RouterType::Pedestrian);
#else
  SetRouter(RouterType::Vehicle);
#endif
  LOG(LDEBUG, ("Routing engine initialized"));

  LOG(LINFO, ("System languages:", languages::GetPreferred()));
}

Framework::~Framework()
{
  m_drapeEngine.reset();

  m_storageBridge.reset();
  m_activeMaps.reset();
  m_model.SetOnMapDeregisteredCallback(nullptr);
}

void Framework::DrawSingleFrame(m2::PointD const & center, int zoomModifier,
                                uint32_t pxWidth, uint32_t pxHeight, FrameImage & image,
                                SingleFrameSymbols const & symbols)
{
  ASSERT(IsSingleFrameRendererInited(), ());
  Navigator frameNavigator = m_navigator;
  frameNavigator.OnSize(0, 0, pxWidth, pxHeight);
  frameNavigator.SetAngle(0);

  m2::RectD rect = m_scales.GetRectForDrawScale(scales::GetUpperComfortScale() - 1, center);
  if (symbols.m_showSearchResult && !rect.IsPointInside(symbols.m_searchResult))
  {
    double const kScaleFactor = 1.3;
    m2::PointD oldCenter = rect.Center();
    rect.Add(symbols.m_searchResult);
    double const centersDiff = 2 * (rect.Center() - oldCenter).Length();

    m2::RectD resultRect;
    resultRect.SetSizes(rect.SizeX() + centersDiff, rect.SizeY() + centersDiff);
    resultRect.SetCenter(center);
    resultRect.Scale(kScaleFactor);
    rect = resultRect;
    ASSERT(rect.IsPointInside(symbols.m_searchResult), ());
  }


  int baseZoom = m_scales.GetDrawTileScale(rect);
  int resultZoom = baseZoom + zoomModifier;
  int const minZoom = symbols.m_bottomZoom == -1 ? resultZoom : symbols.m_bottomZoom;
  resultZoom = my::clamp(resultZoom, minZoom, scales::GetUpperScale());
  rect = m_scales.GetRectForDrawScale(resultZoom, rect.Center());

  CheckMinGlobalRect(rect);
  CheckMinMaxVisibleScale(rect);
  frameNavigator.SetFromRect(m2::AnyRectD(rect));

  m_cpuDrawer->BeginFrame(pxWidth, pxHeight);

  ScreenBase const & s = frameNavigator.Screen();
  shared_ptr<PaintEvent> event = make_shared<PaintEvent>(m_cpuDrawer.get());
  DrawModel(event, s, m2::RectD(0, 0, pxWidth, pxHeight), m_scales.GetTileScaleBase(s), false);

  m_cpuDrawer->Flush();
  m_cpuDrawer->DrawMyPosition(frameNavigator.GtoP(center));

  if (symbols.m_showSearchResult)
  {
    if (!frameNavigator.Screen().PixelRect().IsPointInside(frameNavigator.GtoP(symbols.m_searchResult)))
      m_cpuDrawer->DrawSearchArrow(ang::AngleTo(rect.Center(), symbols.m_searchResult));
    else
      m_cpuDrawer->DrawSearchResult(frameNavigator.GtoP(symbols.m_searchResult));
  }

  m_cpuDrawer->EndFrame(image);
}

void Framework::InitSingleFrameRenderer(graphics::EDensity density)
{
  ASSERT(!IsSingleFrameRendererInited(), ());
  if (m_cpuDrawer == nullptr)
  {
    CPUDrawer::Params params(GetGlyphCacheParams(density));
    params.m_visualScale = graphics::visualScale(density);
    params.m_density = density;

    m_cpuDrawer.reset(new CPUDrawer(params));
  }
}

void Framework::ReleaseSingleFrameRenderer()
{
  if (IsSingleFrameRendererInited())
    m_cpuDrawer.reset();
}

bool Framework::IsSingleFrameRendererInited() const
{
  return m_cpuDrawer != nullptr;
}

void Framework::DeleteCountry(storage::TIndex const & index, TMapOptions opt)
{
  switch (opt)
  {
    case TMapOptions::ENothing:
      return;
    case TMapOptions::EMap:  // fall through
    case TMapOptions::EMapWithCarRouting:
    {
      CountryFile const & countryFile = m_storage.GetCountryFile(index);
      // m_model will notify us when latest map file will be deleted via
      // OnMapDeregistered call.
      if (m_model.DeregisterMap(countryFile))
      {
        ///@TODO UVR
        //InvalidateRect(GetCountryBounds(countryFile.GetNameWithoutExt()), true /* doForceUpdate */);
      }
      // TODO (@ldragunov, @gorshenin): rewrite routing session to use MwmHandles. Thus,
      // it won' be needed to reset it after maps update.
      m_routingSession.Reset();
      return;
    }
    case TMapOptions::ECarRouting:
      m_routingSession.Reset();
      m_storage.DeleteCountry(index, opt);
      return;
  }
}

void Framework::DownloadCountry(TIndex const & index, TMapOptions opt)
{
  m_storage.DownloadCountry(index, opt);
}

TStatus Framework::GetCountryStatus(TIndex const & index) const
{
  return m_storage.CountryStatusEx(index);
}

string Framework::GetCountryName(TIndex const & index) const
{
  string group, name;
  m_storage.GetGroupAndCountry(index, group, name);
  return (!group.empty() ? group + ", " + name : name);
}

m2::RectD Framework::GetCountryBounds(string const & file) const
{
  m2::RectD const r = GetSearchEngine()->GetCountryBounds(file);
  ASSERT ( r.IsValid(), () );
  return r;
}

m2::RectD Framework::GetCountryBounds(TIndex const & index) const
{
  CountryFile const & file = m_storage.GetCountryFile(index);
  return GetCountryBounds(file.GetNameWithoutExt());
}

void Framework::ShowCountry(TIndex const & index)
{
  StopLocationFollow();

  ShowRect(GetCountryBounds(index));
}

void Framework::UpdateAfterDownload(LocalCountryFile const & localFile)
{
  // TODO (@ldragunov, @gorshenin): rewrite routing session to use MwmHandles. Thus,
  // it won' be needed to reset it after maps update.
  m_routingSession.Reset();

  if (!HasOptions(localFile.GetFiles(), TMapOptions::EMap))
    return;

  // Add downloaded map.
  auto result = m_model.RegisterMap(localFile);
  MwmSet::MwmHandle const & handle = result.first;
  if (handle.IsAlive())
    //InvalidateRect(handle.GetInfo()->m_limitRect, true /* doForceUpdate */);
  GetSearchEngine()->ClearViewportsCache();
}

void Framework::OnMapDeregistered(platform::LocalCountryFile const & localFile)
{
  m_storage.DeleteCustomCountryVersion(localFile);
}

void Framework::RegisterAllMaps()
{
  ASSERT(!m_storage.IsDownloadInProgress(),
         ("Registering maps while map downloading leads to removing downloading maps from "
          "ActiveMapsListener::m_items."));

  platform::CleanupMapsDirectory();
  m_storage.RegisterAllLocalMaps();

  int minFormat = numeric_limits<int>::max();
  vector<CountryFile> maps;
  m_storage.GetLocalMaps(maps);

  for (CountryFile const & countryFile : maps)
  {
    shared_ptr<LocalCountryFile> localFile = m_storage.GetLatestLocalFile(countryFile);
    if (!localFile)
      continue;
    auto p = RegisterMap(*localFile);
    if (p.second != MwmSet::RegResult::Success)
      continue;
    MwmSet::MwmHandle const & handle = p.first;
    ASSERT(handle.IsAlive(), ());
    minFormat = min(minFormat, static_cast<int>(handle.GetInfo()->m_version.format));
  }

  m_activeMaps->Init(maps);

  GetSearchEngine()->SupportOldFormat(minFormat < version::v3);
}

void Framework::DeregisterAllMaps()
{
  m_activeMaps->Clear();
  m_model.Clear();
  m_storage.Clear();
}

void Framework::LoadBookmarks()
{
  m_bmManager.LoadBookmarks();
}

size_t Framework::AddBookmark(size_t categoryIndex, const m2::PointD & ptOrg, BookmarkData & bm)
{
  return m_bmManager.AddBookmark(categoryIndex, ptOrg, bm);
}

size_t Framework::MoveBookmark(size_t bmIndex, size_t curCatIndex, size_t newCatIndex)
{
  return m_bmManager.MoveBookmark(bmIndex, curCatIndex, newCatIndex);
}

void Framework::ReplaceBookmark(size_t catIndex, size_t bmIndex, BookmarkData const & bm)
{
  m_bmManager.ReplaceBookmark(catIndex, bmIndex, bm);
}

size_t Framework::AddCategory(string const & categoryName)
{
  return m_bmManager.CreateBmCategory(categoryName);
}

namespace
{
  class EqualCategoryName
  {
    string const & m_name;
  public:
    EqualCategoryName(string const & name) : m_name(name) {}
    bool operator() (BookmarkCategory const * cat) const
    {
      return (cat->GetName() == m_name);
    }
  };
}

BookmarkCategory * Framework::GetBmCategory(size_t index) const
{
  return m_bmManager.GetBmCategory(index);
}

bool Framework::DeleteBmCategory(size_t index)
{
  return m_bmManager.DeleteBmCategory(index);
}

void Framework::ShowBookmark(BookmarkAndCategory const & bnc)
{
  StopLocationFollow();

  // show ballon above
  Bookmark const * mark = static_cast<Bookmark const *>(GetBmCategory(bnc.first)->GetUserMark(bnc.second));

  double scale = mark->GetScale();
  if (scale == -1.0)
    scale = scales::GetUpperComfortScale();

  CallDrapeFunction(bind(&df::DrapeEngine::SetModelViewCenter, _1, mark->GetPivot(), scale, true));
  ActivateUserMark(mark, true);
}

void Framework::ShowTrack(Track const & track)
{
  StopLocationFollow();
  ShowRect(track.GetLimitRect());
}

void Framework::ClearBookmarks()
{
  m_bmManager.ClearItems();
}

namespace
{

/// @return extension with a dot in lower case
string const GetFileExt(string const & filePath)
{
  string ext = my::GetFileExtension(filePath);
  transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  return ext;
}

string const GetFileName(string const & filePath)
{
  string ret = filePath;
  my::GetNameFromFullPath(ret);
  return ret;
}

string const GenerateValidAndUniqueFilePathForKML(string const & fileName)
{
  string filePath = BookmarkCategory::RemoveInvalidSymbols(fileName);
  filePath = BookmarkCategory::GenerateUniqueFileName(GetPlatform().SettingsDir(), filePath);
  return filePath;
}

} // namespace

bool Framework::AddBookmarksFile(string const & filePath)
{
  string const fileExt = GetFileExt(filePath);
  string fileSavePath;
  if (fileExt == BOOKMARKS_FILE_EXTENSION)
  {
    fileSavePath = GenerateValidAndUniqueFilePathForKML(GetFileName(filePath));
    if (!my::CopyFileX(filePath, fileSavePath))
      return false;
  }
  else if (fileExt == KMZ_EXTENSION)
  {
    try
    {
      ZipFileReader::FileListT files;
      ZipFileReader::FilesList(filePath, files);
      string kmlFileName;
      for (size_t i = 0; i < files.size(); ++i)
      {
        if (GetFileExt(files[i].first) == BOOKMARKS_FILE_EXTENSION)
        {
          kmlFileName = files[i].first;
          break;
        }
      }
      if (kmlFileName.empty())
        return false;

      fileSavePath = GenerateValidAndUniqueFilePathForKML(kmlFileName);
      ZipFileReader::UnzipFile(filePath, kmlFileName, fileSavePath);
    }
    catch (RootException const & e)
    {
      LOG(LWARNING, ("Error unzipping file", filePath, e.Msg()));
      return false;
    }
  }
  else
  {
    LOG(LWARNING, ("Unknown file type", filePath));
    return false;
  }

  // Update freshly added bookmarks
  m_bmManager.LoadBookmark(fileSavePath);

  return true;
}

void Framework::PrepareToShutdown()
{
  DestroyDrapeEngine();
}

void Framework::SaveState()
{
  Settings::Set("ScreenClipRect", m_currentMovelView.GlobalRect());
}

void Framework::LoadState()
{
  m2::AnyRectD rect;
  if (Settings::Get("ScreenClipRect", rect) &&
      df::GetWorldRect().IsRectInside(rect.GetGlobalRect()))
  {
    CallDrapeFunction(bind(&df::DrapeEngine::SetModelViewAnyRect, _1, rect, false));
  }
  else
    ShowAll();
}

void Framework::ShowAll()
{
  CallDrapeFunction(bind(&df::DrapeEngine::SetModelViewAnyRect, _1, m2::AnyRectD(m_model.GetWorldRect()), false));
}

m2::PointD Framework::GetPixelCenter() const
{
  return m_currentMovelView.PixelRect().Center();
}

m2::PointD const & Framework::GetViewportCenter() const
{
  return m_currentMovelView.GetOrg();
}

void Framework::SetViewportCenter(m2::PointD const & pt)
{
  CallDrapeFunction(bind(&df::DrapeEngine::SetModelViewCenter, _1, pt, -1, true));
}

m2::RectD Framework::GetCurrentViewport() const
{
  return m_currentMovelView.ClipRect();
}

void Framework::ShowRect(double lat, double lon, double zoom)
{
  m2::PointD center(MercatorBounds::FromLatLon(lat, lon));
  CallDrapeFunction(bind(&df::DrapeEngine::SetModelViewCenter, _1, center, zoom, true));
}

void Framework::ShowRect(m2::RectD const & rect, int maxScale)
{
  CallDrapeFunction(bind(&df::DrapeEngine::SetModelViewRect, _1, rect, true, maxScale, true));
}

void Framework::ShowRect(m2::AnyRectD const & rect)
{
  CallDrapeFunction(bind(&df::DrapeEngine::SetModelViewAnyRect, _1, rect, true));
}

void Framework::GetTouchRect(m2::PointD const & center, uint32_t pxRadius, m2::AnyRectD & rect)
{
  m_currentMovelView.GetTouchRect(center, static_cast<double>(pxRadius), rect);
}

int Framework::AddViewportListener(TViewportChanged const & fn)
{
  ASSERT(m_drapeEngine, ());
  return m_drapeEngine->AddModelViewListener(fn);
}

void Framework::RemoveViewportListener(int slotID)
{
  ASSERT(m_drapeEngine, ());
  m_drapeEngine->RemoveModeViewListener(slotID);
}

void Framework::OnSize(int w, int h)
{
  CallDrapeFunction(bind(&df::DrapeEngine::Resize, _1, max(w, 2), max(h, 2)));
}

namespace
{

double ScaleModeToFactor(Framework::EScaleMode mode)
{
  double factors[] = { 2.0, 1.5, 0.5, 0.67 };
  return factors[mode];
}

} // namespace

void Framework::Scale(EScaleMode mode, bool isAnim)
{
  Scale(ScaleModeToFactor(mode), isAnim);
}

void Framework::Scale(Framework::EScaleMode mode, m2::PointD const & pxPoint, bool isAnim)
{
  Scale(ScaleModeToFactor(mode), pxPoint, isAnim);
}

void Framework::Scale(double factor, bool isAnim)
{
  Scale(factor, m_currentMovelView.PixelRect().Center(), isAnim);
}

void Framework::Scale(double factor, m2::PointD const & pxPoint, bool isAnim)
{
  CallDrapeFunction(bind(&df::DrapeEngine::Scale, _1, factor, pxPoint, isAnim));
}

void Framework::TouchEvent(df::TouchEvent const & touch)
{
  m_drapeEngine->AddTouchEvent(touch);
}

int Framework::GetDrawScale() const
{
  return df::GetDrawTileScale(m_currentMovelView);
}

bool Framework::IsCountryLoaded(m2::PointD const & pt) const
{
  // Correct, but slow version (check country polygon).
  string const fName = GetSearchEngine()->GetCountryFile(pt);
  if (fName.empty())
    return true;

  return m_model.IsLoaded(fName);
}

///@TODO UVR
//void Framework::DrawAdditionalInfo(shared_ptr<PaintEvent> const & e)
//{
//  // m_informationDisplay is set and drawn after the m_renderPolicy
//  ASSERT ( m_renderPolicy, () );

//  Drawer * pDrawer = e->drawer();
//  graphics::Screen * pScreen = pDrawer->screen();

//  pScreen->beginFrame();

//  bool const isEmptyModel = m_renderPolicy->IsEmptyModel();

//  if (isEmptyModel)
//    m_informationDisplay.setEmptyCountryIndex(GetCountryIndex(GetViewportCenter()));
//  else
//    m_informationDisplay.setEmptyCountryIndex(storage::TIndex());

//  bool const isCompassEnabled = my::Abs(ang::GetShortestDistance(m_navigator.Screen().GetAngle(), 0.0)) > my::DegToRad(3.0);
//  bool const isCompasActionEnabled = m_informationDisplay.isCompassArrowEnabled() && m_navigator.InAction();

//  m_informationDisplay.enableCompassArrow(isCompassEnabled || isCompasActionEnabled);
//  m_informationDisplay.setCompassArrowAngle(m_navigator.Screen().GetAngle());

//  int const drawScale = GetDrawScale();
//  m_informationDisplay.setDebugInfo(0, drawScale);

//  m_informationDisplay.enableRuler(drawScale > 4 && !m_informationDisplay.isCopyrightActive());

//  pScreen->endFrame();

//  m_bmManager.DrawItems(e);
//  m_guiController->UpdateElements();
//  m_guiController->DrawFrame(pScreen);
//}

void Framework::UpdateUserViewportChanged()
{
  if (IsISActive())
  {
    (void)GetCurrentPosition(m_lastSearch.m_lat, m_lastSearch.m_lon);
    m_lastSearch.m_callback = bind(&Framework::OnSearchResultsCallback, this, _1);
    m_lastSearch.SetSearchMode(search::SearchParams::IN_VIEWPORT_ONLY);
    m_lastSearch.SetForceSearch(false);

    (void)GetSearchEngine()->Search(m_lastSearch, GetCurrentViewport());
  }
}

void Framework::OnSearchResultsCallback(search::Results const & results)
{
  if (!results.IsEndMarker() && results.GetCount() > 0)
  {
    // Got here from search thread. Need to switch into GUI thread to modify search mark container.
    // Do copy the results structure to pass into GUI thread.
    GetPlatform().RunOnGuiThread(bind(&Framework::OnSearchResultsCallbackUI, this, results));
  }
}

void Framework::OnSearchResultsCallbackUI(search::Results const & results)
{
  if (IsISActive())
    FillSearchResultsMarks(results);
}

void Framework::ClearAllCaches()
{
  m_model.ClearCaches();
  GetSearchEngine()->ClearAllCaches();
}

void Framework::OnDownloadMapCallback(storage::TIndex const & countryIndex)
{
  m_activeMaps->DownloadMap(countryIndex, TMapOptions::EMapOnly);
}

void Framework::OnDownloadMapRoutingCallback(storage::TIndex const & countryIndex)
{
  m_activeMaps->DownloadMap(countryIndex, TMapOptions::EMapWithCarRouting);
}

void Framework::OnDownloadRetryCallback(storage::TIndex const & countryIndex)
{
  m_activeMaps->RetryDownloading(countryIndex);
}

void Framework::OnUpdateCountryIndex(storage::TIndex const & currentIndex, m2::PointF const & pt)
{
  storage::TIndex newCountryIndex = GetCountryIndex(m2::PointD(pt));
  if (currentIndex != newCountryIndex)
    UpdateCountryInfo(newCountryIndex, true /* isCurrentCountry */);
}

void Framework::UpdateCountryInfo(storage::TIndex const & countryIndex, bool isCurrentCountry)
{
  ASSERT(m_activeMaps != nullptr, ());
  ASSERT(m_drapeEngine != nullptr, ());

  gui::CountryInfo countryInfo;

  countryInfo.m_countryIndex = countryIndex;
  countryInfo.m_currentCountryName = m_activeMaps->GetFormatedCountryName(countryIndex);
  countryInfo.m_mapSize = m_activeMaps->GetRemoteCountrySizes(countryIndex).first;
  countryInfo.m_routingSize = m_activeMaps->GetRemoteCountrySizes(countryIndex).second;
  countryInfo.m_countryStatus = m_activeMaps->GetCountryStatus(countryIndex);
  if (countryInfo.m_countryStatus == storage::TStatus::EDownloading)
  {
    storage::LocalAndRemoteSizeT progress = m_activeMaps->GetDownloadableCountrySize(countryIndex);
    countryInfo.m_downloadProgress = progress.first * 100 / progress.second;
  }

  string const & fileName = m_storage.CountryByIndex(countryIndex).GetFile().GetFileWithoutExt();
  bool const isLoaded = m_model.IsLoaded(fileName);
  m_drapeEngine->SetCountryInfo(countryInfo, isCurrentCountry, isLoaded);
}

void Framework::MemoryWarning()
{
  LOG(LINFO, ("MemoryWarning"));
  ClearAllCaches();
}

void Framework::EnterBackground()
{
  // Do not clear caches for Android. This function is called when main activity is paused,
  // but at the same time search activity (for example) is enabled.
#ifndef OMIM_OS_ANDROID
  ClearAllCaches();
#endif

  dlg_settings::EnterBackground(my::Timer::LocalTime() - m_StartForegroundTime);

  ASSERT(m_drapeEngine != nullptr, ("Drape engine has not been initialized yet"));
  if (m_drapeEngine != nullptr)
    m_drapeEngine->SetRenderingEnabled(false);
}

void Framework::EnterForeground()
{
  m_StartForegroundTime = my::Timer::LocalTime();

  ASSERT(m_drapeEngine != nullptr, ("Drape engine has not been initialized yet"));
  if (m_drapeEngine != nullptr)
    m_drapeEngine->SetRenderingEnabled(true);
}

/// @name Drag implementation.
//@{

//void Framework::StartDrag(DragEvent const & e)
//{
//  m_navigator.StartDrag(m_navigator.ShiftPoint(e.Pos()), ElapsedSeconds());
//  ///@TODO UVR
//  //m_informationDisplay.locationState()->DragStarted();
//}

//void Framework::DoDrag(DragEvent const & e)
//{
//  m_navigator.DoDrag(m_navigator.ShiftPoint(e.Pos()), ElapsedSeconds());
//}

//void Framework::StopDrag(DragEvent const & e)
//{
//  m_navigator.StopDrag(m_navigator.ShiftPoint(e.Pos()), ElapsedSeconds(), true);
//  ///@TODO UVR
//  //m_informationDisplay.locationState()->DragEnded();
//}

//@}

/// @name Scaling.
//@{
//void Framework::ScaleToPoint(ScaleToPointEvent const & e, bool anim)
//{
  //m2::PointD pt = m_navigator.ShiftPoint(e.Pt());
  ///@TODO UVR
  //GetLocationState()->CorrectScalePoint(pt);
  //m_navigator.ScaleToPoint(pt, e.ScaleFactor(), 0);
//  UpdateUserViewportChanged();
//}

//void Framework::ScaleDefault(bool enlarge)
//{
//  Scale(enlarge ? 1.5 : 2.0/3.0);
//}

//void Framework::Scale(double scale)
//{
  //m2::PointD center = GetPixelCenter();
  ///@TODO UVR
  //GetLocationState()->CorrectScalePoint(center);
  //m_navigator.ScaleToPoint(center, scale, 0.0);

//  UpdateUserViewportChanged();
//}

//void Framework::StartScale(ScaleEvent const & e)
//{
//  m2::PointD pt1, pt2;
//  CalcScalePoints(e, pt1, pt2);

  ///@TODO UVR
  //GetLocationState()->ScaleStarted();
  //m_navigator.StartScale(pt1, pt2, ElapsedSeconds());
//}

//void Framework::DoScale(ScaleEvent const & e)
//{
//  m2::PointD pt1, pt2;
//  CalcScalePoints(e, pt1, pt2);

  //m_navigator.DoScale(pt1, pt2, ElapsedSeconds());

  ///@TODO UVR
//  if (m_navigator.IsRotatingDuringScale())
//    GetLocationState()->Rotated();
//}

//void Framework::StopScale(ScaleEvent const & e)
//{
  //m2::PointD pt1, pt2;
  //CalcScalePoints(e, pt1, pt2);

  //m_navigator.StopScale(pt1, pt2, ElapsedSeconds());
  //UpdateUserViewportChanged();

  ///@TODO UVR
  //GetLocationState()->ScaleEnded();
//}
//@}

search::Engine * Framework::GetSearchEngine() const
{
  if (!m_pSearchEngine)
  {
    Platform & pl = GetPlatform();

    try
    {
      m_pSearchEngine.reset(new search::Engine(
          &m_model.GetIndex(), pl.GetReader(SEARCH_CATEGORIES_FILE_NAME),
          pl.GetReader(PACKED_POLYGONS_FILE), pl.GetReader(COUNTRIES_FILE),
          languages::GetCurrentOrig(), make_unique<search::SearchQueryFactory>()));
    }
    catch (RootException const & e)
    {
      LOG(LCRITICAL, ("Can't load needed resources for search::Engine: ", e.Msg()));
    }
  }

  return m_pSearchEngine.get();
}

TIndex Framework::GetCountryIndex(m2::PointD const & pt) const
{
  return m_storage.FindIndexByFile(GetSearchEngine()->GetCountryFile(pt));
}

string Framework::GetCountryName(m2::PointD const & pt) const
{
  return GetSearchEngine()->GetCountryName(pt);
}

string Framework::GetCountryName(string const & id) const
{
  return GetSearchEngine()->GetCountryName(id);
}

void Framework::PrepareSearch()
{
  GetSearchEngine()->PrepareSearch(GetCurrentViewport());
}

bool Framework::Search(search::SearchParams const & params)
{
  if (params.m_query == ROUTING_SECRET_UNLOCKING_WORD)
  {
    LOG(LINFO, ("Pedestrian routing mode enabled"));
    SetRouter(RouterType::Pedestrian);
    return false;
  }
  if (params.m_query == ROUTING_SECRET_LOCKING_WORD)
  {
    LOG(LINFO, ("Vehicle routing mode enabled"));
    SetRouter(RouterType::Vehicle);
    return false;
  }

#ifdef FIXED_LOCATION
  search::SearchParams rParams(params);
  if (params.IsValidPosition())
  {
    m_fixedPos.GetLat(rParams.m_lat);
    m_fixedPos.GetLon(rParams.m_lon);
  }
#else
  search::SearchParams const & rParams = params;
#endif

  return GetSearchEngine()->Search(rParams, GetCurrentViewport());
}

bool Framework::GetCurrentPosition(double & lat, double & lon) const
{
  ///@TODO UVR
  //shared_ptr<State> const & locationState = m_informationDisplay.locationState();

//  if (locationState->IsModeHasPosition())
//  {
//    m2::PointD const pos = locationState->Position();
//    lat = MercatorBounds::YToLat(pos.y);
//    lon = MercatorBounds::XToLon(pos.x);
//    return true;
//  }
//  else
    return false;
}

void Framework::ShowSearchResult(search::Result const & res)
{
  UserMarkControllerGuard guard(m_bmManager, UserMarkType::SEARCH_MARK);
  guard.m_controller.SetIsDrawable(false);
  guard.m_controller.Clear();
  guard.m_controller.SetIsVisible(true);

  m_lastSearch.Clear();
  m_fixedSearchResults = 0;

  int scale;
  m2::PointD center;

  using namespace search;
  using namespace feature;

  switch (res.GetResultType())
  {
    case Result::RESULT_FEATURE:
    {
      FeatureID const id = res.GetFeatureID();
      Index::FeaturesLoaderGuard guard(m_model.GetIndex(), id.m_mwmId);

      FeatureType ft;
      guard.GetFeature(id.m_offset, ft);

      scale = GetFeatureViewportScale(TypesHolder(ft));
      center = GetCenter(ft, scale);
      break;
    }

    case Result::RESULT_LATLON:
    case Result::RESULT_ADDRESS:
      scale = scales::GetUpperComfortScale();
      center = res.GetFeatureCenter();
      break;

    default:
      return;
  }

  StopLocationFollow();
  ShowRect(df::GetRectForDrawScale(scale, center));

  search::AddressInfo info;
  info.MakeFrom(res);

  SearchMarkPoint * mark = static_cast<SearchMarkPoint *>(guard.m_controller.CreateUserMark(center));
  mark->SetInfo(info);

  ActivateUserMark(mark, false);
}

size_t Framework::ShowAllSearchResults()
{
  using namespace search;

  Results results;
  GetSearchEngine()->GetResults(results);

  size_t count = results.GetCount();
  switch (count)
  {
  case 1:
    {
      Result const & r = results.GetResult(0);
      if (!r.IsSuggest())
        ShowSearchResult(r);
      else
        count = 0;
      // do not put break here
    }
  case 0:
    return count;
  }

  m_fixedSearchResults = 0;
  FillSearchResultsMarks(results);
  m_fixedSearchResults = count;

  ///@TODO UVR
  //shared_ptr<State> state = GetLocationState();
  //state->SetFixedZoom();
  // Setup viewport according to results.
  m2::AnyRectD viewport = m_currentMovelView.GlobalRect();
  m2::PointD const center = viewport.Center();

  double minDistance = numeric_limits<double>::max();
  int minInd = -1;
  for (size_t i = 0; i < count; ++i)
  {
    Result const & r = results.GetResult(i);
    if (r.HasPoint())
    {
      double const dist = center.SquareLength(r.GetFeatureCenter());
      if (dist < minDistance)
      {
        minDistance = dist;
        minInd = static_cast<int>(i);
      }
    }
  }

  if (minInd != -1)
  {
    m2::PointD const pt = results.GetResult(minInd).GetFeatureCenter();
    if (!viewport.IsPointInside(pt))
    {
      viewport.SetSizesToIncludePoint(pt);

      ShowRect(viewport);
      ///@TODO UVR
      //StopLocationFollow();
    }
  }

  return count;
}

void Framework::FillSearchResultsMarks(search::Results const & results)
{
  UserMarkControllerGuard guard(m_bmManager, UserMarkType::SEARCH_MARK);
  guard.m_controller.SetIsVisible(true);
  guard.m_controller.SetIsDrawable(true);
  guard.m_controller.Clear(m_fixedSearchResults);

  size_t const count = results.GetCount();
  for (size_t i = 0; i < count; ++i)
  {
    using namespace search;

    Result const & r = results.GetResult(i);
    if (r.HasPoint())
    {
      AddressInfo info;
      info.MakeFrom(r);

      m2::PointD const pt = r.GetFeatureCenter();
      SearchMarkPoint * mark = static_cast<SearchMarkPoint *>(guard.m_controller.CreateUserMark(pt));
      mark->SetInfo(info);
    }
  }
}

void Framework::CancelInteractiveSearch()
{
  m_lastSearch.Clear();
  UserMarkControllerGuard(m_bmManager, UserMarkType::SEARCH_MARK).m_controller.Clear();
  m_fixedSearchResults = 0;
}

bool Framework::GetDistanceAndAzimut(m2::PointD const & point,
                                     double lat, double lon, double north,
                                     string & distance, double & azimut)
{
#ifdef FIXED_LOCATION
  m_fixedPos.GetLat(lat);
  m_fixedPos.GetLon(lon);
  m_fixedPos.GetNorth(north);
#endif

  double const d = ms::DistanceOnEarth(lat, lon,
                                       MercatorBounds::YToLat(point.y),
                                       MercatorBounds::XToLon(point.x));

  // Distance may be less than 1.0
  (void) MeasurementUtils::FormatDistance(d, distance);

  if (north >= 0.0)
  {
    // We calculate azimut even when distance is very short (d ~ 0),
    // because return value has 2 states (near me or far from me).

    azimut = ang::AngleTo(MercatorBounds::FromLatLon(lat, lon), point) + north;

    double const pi2 = 2.0*math::pi;
    if (azimut < 0.0)
      azimut += pi2;
    else if (azimut > pi2)
      azimut -= pi2;
  }

  // This constant and return value is using for arrow/flag choice.
  return (d < 25000.0);
}

void Framework::CreateDrapeEngine(ref_ptr<dp::OGLContextFactory> contextFactory, DrapeCreationParams && params)
{
  using TReadIDsFn = df::MapDataProvider::TReadIDsFn;
  using TReadFeaturesFn = df::MapDataProvider::TReadFeaturesFn;
  using TUpdateCountryIndexFn = df::MapDataProvider::TUpdateCountryIndexFn;
  using TIsCountryLoadedFn = df::MapDataProvider::TIsCountryLoadedFn;
  using TDownloadFn = df::MapDataProvider::TDownloadFn;

  TReadIDsFn idReadFn = [this](df::MapDataProvider::TReadCallback<FeatureID> const & fn, m2::RectD const & r, int scale) -> void
  {
    m_model.ForEachFeatureID(r, fn, scale);
  };

  TReadFeaturesFn featureReadFn = [this](df::MapDataProvider::TReadCallback<FeatureType> const & fn, vector<FeatureID> const & ids) -> void
  {
    m_model.ReadFeatures(fn, ids);
  };

  TUpdateCountryIndexFn updateCountryIndex = [this](storage::TIndex const & currentIndex, m2::PointF const & pt)
  {
    GetPlatform().RunOnGuiThread(bind(&Framework::OnUpdateCountryIndex, this, currentIndex, pt));
  };

  TIsCountryLoadedFn isCountryLoadedFn = bind(&Framework::IsCountryLoaded, this, _1);

  TDownloadFn downloadMapFn = [this](storage::TIndex const & countryIndex)
  {
    GetPlatform().RunOnGuiThread(bind(&Framework::OnDownloadMapCallback, this, countryIndex));
  };

  TDownloadFn downloadMapRoutingFn = [this](storage::TIndex const & countryIndex)
  {
    GetPlatform().RunOnGuiThread(bind(&Framework::OnDownloadMapRoutingCallback, this, countryIndex));
  };

  TDownloadFn downloadRetryFn = [this](storage::TIndex const & countryIndex)
  {
    GetPlatform().RunOnGuiThread(bind(&Framework::OnDownloadRetryCallback, this, countryIndex));
  };

  df::DrapeEngine::Params p(contextFactory,
                            make_ref(&m_stringsBundle),
                            df::Viewport(0, 0, params.m_surfaceWidth, params.m_surfaceHeight),
                            df::MapDataProvider(idReadFn, featureReadFn, updateCountryIndex, isCountryLoadedFn,
                                                downloadMapFn, downloadMapRoutingFn, downloadRetryFn),
                            params.m_visualScale,
                            move(params.m_widgetsInitInfo));

  m_drapeEngine = make_unique_dp<df::DrapeEngine>(move(p));
  AddViewportListener([this](ScreenBase const & screen)
  {
    m_currentMovelView = screen;
  });
  m_drapeEngine->SetTapEventInfoListener(bind(&Framework::OnTapEvent, this, _1, _2, _3, _4));
  m_drapeEngine->SetUserPositionListener(bind(&Framework::OnUserPositionChanged, this, _1));
  OnSize(params.m_surfaceWidth, params.m_surfaceHeight);
}

ref_ptr<df::DrapeEngine> Framework::GetDrapeEngine()
{
  return make_ref(m_drapeEngine);
}

void Framework::DestroyDrapeEngine()
{
  m_drapeEngine.reset();
}

void Framework::SetMapStyle(MapStyle mapStyle)
{
  GetStyleReader().SetCurrentStyle(mapStyle);
  drule::LoadRules();
}

MapStyle Framework::GetMapStyle() const
{
  return GetStyleReader().GetCurrentStyle();
}

void Framework::SetWidgetLayout(gui::TWidgetsLayoutInfo && layout)
{
  ASSERT(m_drapeEngine != nullptr, ());
  m_drapeEngine->SetWidgetLayout(move(layout));
}

gui::TWidgetsSizeInfo const & Framework::GetWidgetSizes()
{
  ASSERT(m_drapeEngine != nullptr, ());
  return m_drapeEngine->GetWidgetSizes();
}

string Framework::GetCountryCode(m2::PointD const & pt) const
{
  return GetSearchEngine()->GetCountryCode(pt);
}

bool Framework::ShowMapForURL(string const & url)
{
  m2::PointD point;
  m2::RectD rect;
  string name;
  UserMark const * apiMark = 0;

  enum ResultT { FAILED, NEED_CLICK, NO_NEED_CLICK };
  ResultT result = FAILED;

  // always hide current balloon here
  DiactivateUserMark();

  using namespace url_scheme;
  using namespace strings;

  if (StartsWith(url, "ge0"))
  {
    Ge0Parser parser;
    double zoom;
    ApiPoint pt;

    if (parser.Parse(url, pt, zoom))
    {
      point = MercatorBounds::FromLatLon(pt.m_lat, pt.m_lon);
      rect = df::GetRectForDrawScale(zoom, point);
      name = pt.m_name;
      result = NEED_CLICK;
    }
  }
  else if (StartsWith(url, "mapswithme://") || StartsWith(url, "mwm://"))
  {
    UserMarkControllerGuard guard(m_bmManager, UserMarkType::API_MARK);
    guard.m_controller.Clear();

    apiMark = url_scheme::ParseUrl(guard.m_controller, url, m_ParsedMapApi, rect);

    if (m_ParsedMapApi.m_isValid)
    {
      guard.m_controller.SetIsVisible(true);
      guard.m_controller.SetIsDrawable(true);
      if (apiMark)
        result = NEED_CLICK;
      else
        result = NO_NEED_CLICK;
    }
    else
      guard.m_controller.SetIsVisible(false);
  }
  else  // Actually, we can parse any geo url scheme with correct coordinates.
  {
    Info info;
    ParseGeoURL(url, info);
    if (info.IsValid())
    {
      point = MercatorBounds::FromLatLon(info.m_lat, info.m_lon);
      rect = df::GetRectForDrawScale(info.m_zoom, point);
      result = NEED_CLICK;
    }
  }

  if (result != FAILED)
  {
    // set viewport and stop follow mode if any
    StopLocationFollow();
    ShowRect(rect);

    if (result != NO_NEED_CLICK)
    {
      if (apiMark)
      {
        LOG(LINFO, ("Show API mark:", static_cast<ApiMarkPoint const *>(apiMark)->GetName()));

        ActivateUserMark(apiMark, false);
      }
      else
      {
        PoiMarkPoint * mark = GetAddressMark(point);
        if (!name.empty())
          mark->SetName(name);
        ActivateUserMark(mark, false);
      }
    }
    else
      DiactivateUserMark();

    return true;
  }
  else
    return false;
}

bool Framework::GetVisiblePOI(m2::PointD const & glbPoint, search::AddressInfo & info, feature::Metadata & metadata) const
{
  ASSERT(m_drapeEngine != nullptr, ());
  FeatureID id = m_drapeEngine->GetVisiblePOI(glbPoint);
  if (!id.IsValid())
    return false;

  GetVisiblePOI(id, info, metadata);
  return true;
}

m2::PointD Framework::GetVisiblePOI(FeatureID id, search::AddressInfo & info, feature::Metadata & metadata) const
{
  ASSERT(id.IsValid(), ());
  Index::FeaturesLoaderGuard guard(m_model.GetIndex(), id.m_mwmId);

  FeatureType ft;
  guard.GetFeature(id.m_offset, ft);

  ft.ParseMetadata();
  metadata = ft.GetMetadata();

  ASSERT_NOT_EQUAL(ft.GetFeatureType(), feature::GEOM_LINE, ());
  m2::PointD const center = feature::GetCenter(ft);

  GetAddressInfo(ft, center, info);

  return GtoP(center);
}

namespace
{

/// POI - is a point or area feature.
class DoFindClosestPOI
{
  m2::PointD const & m_pt;
  double m_distMeters;
  FeatureID m_id;

public:
  DoFindClosestPOI(m2::PointD const & pt, double tresholdMeters)
    : m_pt(pt), m_distMeters(tresholdMeters)
  {
  }

  void operator() (FeatureType & ft)
  {
    if (ft.GetFeatureType() == feature::GEOM_LINE)
      return;

    double const dist = MercatorBounds::DistanceOnEarth(m_pt, feature::GetCenter(ft));
    if (dist < m_distMeters)
    {
      m_distMeters = dist;
      m_id = ft.GetID();
    }
  }

  void LoadMetadata(model::FeaturesFetcher const & model, feature::Metadata & metadata) const
  {
    if (!m_id.IsValid())
      return;

    Index::FeaturesLoaderGuard guard(model.GetIndex(), m_id.m_mwmId);

    FeatureType ft;
    guard.GetFeature(m_id.m_offset, ft);

    ft.ParseMetadata();
    metadata = ft.GetMetadata();
  }
};

}

void Framework::FindClosestPOIMetadata(m2::PointD const & pt, feature::Metadata & metadata) const
{
  m2::RectD rect(pt, pt);
  double const inf = MercatorBounds::GetCellID2PointAbsEpsilon();
  rect.Inflate(inf, inf);

  DoFindClosestPOI doFind(pt, 1.1 /* search radius in meters */);
  m_model.ForEachFeature(rect, doFind, scales::GetUpperScale() /* scale level for POI */);

  doFind.LoadMetadata(m_model, metadata);
}

BookmarkAndCategory Framework::FindBookmark(UserMark const * mark) const
{
  BookmarkAndCategory empty = MakeEmptyBookmarkAndCategory();
  BookmarkAndCategory result = empty;
  for (size_t i = 0; i < GetBmCategoriesCount(); ++i)
  {
    if (mark->GetContainer() == GetBmCategory(i))
    {
      result.first = i;
      break;
    }
  }

  ASSERT(result.first != empty.first, ());
  BookmarkCategory const * cat = GetBmCategory(result.first);
  for (size_t i = 0; i < cat->GetUserMarkCount(); ++i)
  {
    if (mark == cat->GetUserMark(i))
    {
      result.second = i;
      break;
    }
  }

  ASSERT(result != empty, ());
  return result;
}

PoiMarkPoint * Framework::GetAddressMark(m2::PointD const & globalPoint) const
{
  search::AddressInfo info;
  GetAddressInfoForGlobalPoint(globalPoint, info);
  PoiMarkPoint * mark = UserMarkContainer::UserMarkForPoi();
  mark->SetPtOrg(globalPoint);
  mark->SetInfo(info);
  return mark;
}

void Framework::ActivateUserMark(UserMark const * mark, bool needAnim)
{
  static UserMark const * activeMark = nullptr;
  if (m_activateUserMarkFn)
  {
    bool hasActive = activeMark != nullptr;
    activeMark = mark;
    if (mark)
    {
      m_activateUserMarkFn(mark->Copy());
      m2::PointD pt = mark->GetPivot();
      df::SelectionShape::ESelectedObject object = df::SelectionShape::OBJECT_USER_MARK;
      UserMark::Type type = mark->GetMarkType();
      if (type == UserMark::Type::MY_POSITION)
        object = df::SelectionShape::OBJECT_MY_POSITION;
      else if (type == UserMark::Type::POI)
        object = df::SelectionShape::OBJECT_POI;

      CallDrapeFunction(bind(&df::DrapeEngine::SelectObject, _1, object, pt, needAnim));
    }
    else
    {
      if (hasActive)
      {
        m_activateUserMarkFn(nullptr);
        CallDrapeFunction(bind(&df::DrapeEngine::DeselectObject, _1));
      }
    }
  }
}

void Framework::DiactivateUserMark()
{
  ActivateUserMark(nullptr, true);
}

void Framework::OnTapEvent(m2::PointD pxPoint, bool isLong, bool isMyPosition, FeatureID feature)
{
  UserMark const * mark = OnTapEventImpl(pxPoint, isLong, isMyPosition, feature);

  {
    alohalytics::TStringMap details {{"isLongPress", isLong ? "1" : "0"}};
    if (mark)
      mark->FillLogEvent(details);
    alohalytics::Stats::Instance().LogEvent("$GetUserMark", details);
  }

  ActivateUserMark(mark, true);
}

UserMark const * Framework::OnTapEventImpl(m2::PointD pxPoint, bool isLong, bool isMyPosition, FeatureID feature)
{
  if (isMyPosition)
  {
    search::AddressInfo info;
    info.m_name = m_stringsBundle.GetString("my_position");
    MyPositionMarkPoint * myPostition = UserMarkContainer::UserMarkForMyPostion();
    myPostition->SetInfo(info);

    return myPostition;
  }

  df::VisualParams const & vp = df::VisualParams::Instance();

  m2::AnyRectD rect;
  uint32_t const touchRadius = vp.GetTouchRectRadius();
  m_currentMovelView.GetTouchRect(pxPoint, touchRadius, rect);

  m2::AnyRectD bmSearchRect;
  double const bmAddition = BM_TOUCH_PIXEL_INCREASE * vp.GetVisualScale();
  double const pxWidth  =  touchRadius;
  double const pxHeight = touchRadius + bmAddition;
  m_currentMovelView.GetTouchRect(pxPoint + m2::PointD(0, bmAddition),
                                  pxWidth, pxHeight, bmSearchRect);
  UserMark const * mark = m_bmManager.FindNearestUserMark(
        [&rect, &bmSearchRect](UserMarkType type) -> m2::AnyRectD const &
        {
          return (type == UserMarkContainer::BOOKMARK_MARK ? bmSearchRect : rect);
        });

  if (mark != nullptr)
    return mark;

  bool needMark = false;
  m2::PointD pxPivot;
  search::AddressInfo info;
  feature::Metadata metadata;

  if (feature.IsValid())
  {
    pxPivot = GetVisiblePOI(feature, info, metadata);
    needMark = true;
  }
  else if (isLong)
  {
    GetAddressInfoForPixelPoint(pxPoint, info);
    pxPivot = pxPoint;
    needMark = true;
  }

  if (needMark)
  {
    PoiMarkPoint * poiMark = UserMarkContainer::UserMarkForPoi();
    poiMark->SetPtOrg(m_currentMovelView.PtoG(pxPivot));
    poiMark->SetInfo(info);
    poiMark->SetMetadata(metadata);
    return poiMark;
  }

  return nullptr;
}

void Framework::PredictLocation(double & lat, double & lon, double accuracy,
                                double bearing, double speed, double elapsedSeconds)
{
  double offsetInM = speed * elapsedSeconds;
  double angle = my::DegToRad(90.0 - bearing);

  m2::PointD mercatorPt = MercatorBounds::MetresToXY(lon, lat, accuracy).Center();
  mercatorPt = MercatorBounds::GetSmPoint(mercatorPt, offsetInM * cos(angle), offsetInM * sin(angle));
  lon = MercatorBounds::XToLon(mercatorPt.x);
  lat = MercatorBounds::YToLat(mercatorPt.y);
}

StringsBundle const & Framework::GetStringsBundle()
{
  return m_stringsBundle;
}

string Framework::CodeGe0url(Bookmark const * bmk, bool addName)
{
  double lat = MercatorBounds::YToLat(bmk->GetPivot().y);
  double lon = MercatorBounds::XToLon(bmk->GetPivot().x);
  return CodeGe0url(lat, lon, bmk->GetScale(), addName ? bmk->GetName() : "");
}

string Framework::CodeGe0url(double lat, double lon, double zoomLevel, string const & name)
{
  size_t const resultSize = MapsWithMe_GetMaxBufferSize(name.size());

  string res(resultSize, 0);
  int const len = MapsWithMe_GenShortShowMapUrl(lat, lon, zoomLevel, name.c_str(), &res[0], res.size());

  ASSERT_LESS_OR_EQUAL(len, res.size(), ());
  res.resize(len);

  return res;
}

string Framework::GenerateApiBackUrl(ApiMarkPoint const & point)
{
  string res = m_ParsedMapApi.m_globalBackUrl;
  if (!res.empty())
  {
    double lat, lon;
    point.GetLatLon(lat, lon);
    res += "pin?ll=" + strings::to_string(lat) + "," + strings::to_string(lon);
    if (!point.GetName().empty())
      res += "&n=" + UrlEncode(point.GetName());
    if (!point.GetID().empty())
      res += "&id=" + UrlEncode(point.GetID());
  }
  return res;
}

bool Framework::IsDataVersionUpdated()
{
  int64_t storedVersion;
  if (Settings::Get("DataVersion", storedVersion))
  {
    return storedVersion < m_storage.GetCurrentDataVersion();
  }
  // no key in the settings, assume new version
  return true;
}

void Framework::UpdateSavedDataVersion()
{
  Settings::Set("DataVersion", m_storage.GetCurrentDataVersion());
}

void Framework::BuildRoute(m2::PointD const & destination)
{
  ASSERT(m_drapeEngine != nullptr, ());

  m2::PointD myPosition;
  bool const hasPosition = m_drapeEngine->GetMyPosition(myPosition);
  if (!hasPosition)
  {
    CallRouteBuilded(IRouter::NoCurrentPosition, vector<storage::TIndex>());
    return;
  }

  if (IsRoutingActive())
    CloseRouting();

  m_routingSession.SetUserCurrentPosition(myPosition);
  m_routingSession.BuildRoute(myPosition, destination,
                              [this] (Route const & route, IRouter::ResultCode code)
  {
    double const routeScale = 1.2;

    vector<storage::TIndex> absentFiles;
    vector<storage::TIndex> absentRoutingIndexes;
    if (code == IRouter::NoError)
    {
      InsertRoute(route);
      m2::RectD routeRect = route.GetPoly().GetLimitRect();
      routeRect.Scale(routeScale);
      m_drapeEngine->SetModelViewRect(routeRect, true, -1, true);
    }
    else
    {
      for (string const & name : route.GetAbsentCountries())
      {
          storage::TIndex fileIndex = m_storage.FindIndexByFile(name);
          if (m_storage.GetLatestLocalFile(fileIndex))
            absentRoutingIndexes.push_back(fileIndex);
          else
            absentCountries.push_back(fileIndex);
        }

        if (code != IRouter::NeedMoreMaps)
          RemoveRoute();

      RemoveRoute(true /* deactivateFollowing */);
    }
    CallRouteBuilded(code, absentFiles);
  });
}

void Framework::FollowRoute()
{
  ASSERT(m_drapeEngine != nullptr, ());
  m_drapeEngine->MyPositionNextMode();

  m2::PointD const & position = m_routingSession.GetUserCurrentPosition();
  m_drapeEngine->SetModelViewCenter(position, scales::GetNavigationScale(), true);
}

void Framework::SetRouter(RouterType type)
{
#ifdef DEBUG
  TRoutingVisualizerFn const routingVisualizerFn = [this](m2::PointD const & pt)
  {
    GetPlatform().RunOnGuiThread([this,pt]()
    {
      UserMarkControllerGuard g(m_bmManager, UserMarkType::DEBUG_MARK);
      g.m_controller.CreateUserMark(pt);
    });
  };
#else
  TRoutingVisualizerFn const routingVisualizerFn = nullptr;
#endif

  auto const routingStatisticsFn = [](map<string, string> const & statistics)
  {
    alohalytics::LogEvent("Routing_CalculatingRoute", statistics);
  };

  auto countryFileGetter = [this](m2::PointD const & p) -> string
  {
    // TODO (@gorshenin): fix search engine to return CountryFile
    // instances instead of plain strings.
    return GetSearchEngine()->GetCountryFile(p);
  };
  auto localFileGetter = [this](string const & countryFile) -> shared_ptr<LocalCountryFile>
  {
    return m_storage.GetLatestLocalFile(CountryFile(countryFile));
  };

  unique_ptr<IRouter> router;
  unique_ptr<OnlineAbsentCountriesFetcher> fetcher;
  if (type == RouterType::Pedestrian)
  {
    router = CreatePedestrianAStarBidirectionalRouter(m_model.GetIndex(), countryFileGetter,
                                                      routingVisualizerFn);
  }
  else
  {
    router.reset(new OsrmRouter(&m_model.GetIndex(), countryFileGetter, localFileGetter,
                                routingVisualizerFn));
    fetcher.reset(new OnlineAbsentCountriesFetcher(countryFileGetter, localFileGetter));
  }

  m_routingSession.SetRouter(move(router), move(fetcher), routingStatisticsFn);
  m_currentRouterType = type;
}

void Framework::RemoveRoute(bool deactivateFollowing)
{
  ASSERT(m_drapeEngine != nullptr, ());
  m_drapeEngine->RemoveRoute(deactivateFollowing);
}

void Framework::CloseRouting()
{
  m_routingSession.Reset();
  RemoveRoute(true /* deactivateFollowing */);
}

void Framework::InsertRoute(Route const & route)
{
  if (route.GetPoly().GetSize() < 2)
  {
    LOG(LWARNING, ("Invalid track - only", route.GetPoly().GetSize(), "point(s)."));
    return;
  }

  ASSERT(m_drapeEngine != nullptr, ());
  m_drapeEngine->AddRoute(route.GetPoly(), dp::Color(110, 180, 240, 200));


  // TODO(@kuznetsov): Maybe we need some of this stuff
  //track.SetName(route.GetName());

  //RouteTrack track(route.GetPoly());
  //track.SetName(route.GetName());
  //track.SetTurnsGeometry(route.GetTurnsGeometry());

  /// @todo Consider a style parameter for the route color.
  //graphics::Color routeColor;
  //if (m_currentRouterType == RouterType::Pedestrian)
  //  routeColor = graphics::Color(5, 105, 175, 255);
  //else
  //  routeColor = graphics::Color(110, 180, 240, 255);

  //Track::TrackOutline outlines[]
  //{
  //  { 10.0f * visScale, routeColor }
  //};

  //track.AddOutline(outlines, ARRAY_SIZE(outlines));
  //track.AddClosingSymbol(false, "route_to", graphics::EPosCenter, graphics::routingFinishDepth);

  //m_informationDisplay.ResetRouteMatchingInfo();
}

void Framework::CheckLocationForRouting(GpsInfo const & info)
{
  if (!IsRoutingActive())
    return;

  RoutingSession::State state = m_routingSession.OnLocationPositionChanged(info);
  if (state == RoutingSession::RouteNeedRebuild)
  {
    m2::PointD const & position = m_routingSession.GetUserCurrentPosition();
    m_routingSession.RebuildRoute(position, [this] (Route const & route, IRouter::ResultCode code)
    {
      if (code == IRouter::NoError)
      {
        RemoveRoute(false /* deactivateFollowing */);
        InsertRoute(route);
      }
    });
  }
  else if (state == RoutingSession::RouteFinished)
  {
    RemoveRoute(false /* deactivateFollowing */);
  }
}

void Framework::MatchLocationToRoute(location::GpsInfo & location, location::RouteMatchingInfo & routeMatchingInfo) const
{
  if (!IsRoutingActive())
    return;

  m_routingSession.MatchLocationToRoute(location, routeMatchingInfo);
}

void Framework::CallRouteBuilded(IRouter::ResultCode code, vector<storage::TIndex> const & absentCountries, vector<storage::TIndex> const & absentRoutingFiles)
{
  if (code == IRouter::Cancelled)
    return;
  m_routingCallback(code, absentCountries, absentRoutingFiles);
}

string Framework::GetRoutingErrorMessage(IRouter::ResultCode code)
{
  string messageID = "";
  switch (code)
  {
  case IRouter::NoCurrentPosition:
    messageID = "routing_failed_unknown_my_position";
    break;
  case IRouter::InconsistentMWMandRoute: // the same as RouteFileNotExist
  case IRouter::RouteFileNotExist:
    messageID = "routing_failed_has_no_routing_file";
    break;
  case IRouter::StartPointNotFound:
    messageID = "routing_failed_start_point_not_found";
    break;
  case IRouter::EndPointNotFound:
    messageID = "routing_failed_dst_point_not_found";
    break;
  case IRouter::PointsInDifferentMWM:
    messageID = "routing_failed_cross_mwm_building";
    break;
  case IRouter::RouteNotFound:
    messageID = "routing_failed_route_not_found";
    break;
  case IRouter::InternalError:
    messageID = "routing_failed_internal_error";
    break;
  default:
    ASSERT(false, ());
  }

  return m_stringsBundle.GetString(messageID);
}
