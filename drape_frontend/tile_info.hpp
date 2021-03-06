#pragma once

#include "drape_frontend/engine_context.hpp"
#include "drape_frontend/memory_feature_index.hpp"
#include "drape_frontend/tile_key.hpp"

#include "indexer/feature_decl.hpp"

#include "base/exception.hpp"

#include "std/atomic.hpp"
#include "std/mutex.hpp"
#include "std/vector.hpp"

class FeatureType;

namespace df
{

class MapDataProvider;
class Stylist;

class TileInfo : private noncopyable
{
public:
  DECLARE_EXCEPTION(ReadCanceledException, RootException);

  TileInfo(drape_ptr<EngineContext> && context);

  void ReadFeatures(MapDataProvider const & model, MemoryFeatureIndex & memIndex);
  void Cancel(MemoryFeatureIndex & memIndex);
  bool IsCancelled() const;

  void Set3dBuildings(bool buildings3d) { m_is3dBuildings = buildings3d; }
  bool Get3dBuildings() const { return m_is3dBuildings; }

  m2::RectD GetGlobalRect() const;
  TileKey const & GetTileKey() const { return m_context->GetTileKey(); }
  bool operator <(TileInfo const & other) const { return GetTileKey() < other.GetTileKey(); }

private:
  void ReadFeatureIndex(MapDataProvider const & model);
  void ProcessID(FeatureID const & id);
  void InitStylist(FeatureType const & f, Stylist & s);
  void CheckCanceled() const;
  bool DoNeedReadIndex() const;

  int GetZoomLevel() const;

private:
  drape_ptr<EngineContext> m_context;
  TFeaturesInfo m_featureInfo;
  bool m_is3dBuildings;

  atomic<bool> m_isCanceled;
};

} // namespace df
