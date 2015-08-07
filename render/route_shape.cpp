#include "render/route_shape.hpp"

#include "base/logging.hpp"

namespace rg
{

namespace
{

float const kLeftSide = 1.0;
float const kCenter = 0.0;
float const kRightSide = -1.0;

enum EPointType
{
  StartPoint = 0,
  EndPoint = 1,
  PointsCount = 2
};

enum ENormalType
{
  StartNormal = 0,
  EndNormal = 1,
  BaseNormal = 2
};

struct LineSegment
{
  m2::PointF m_points[PointsCount];
  m2::PointF m_tangent;
  m2::PointF m_leftBaseNormal;
  m2::PointF m_leftNormals[PointsCount];
  m2::PointF m_rightBaseNormal;
  m2::PointF m_rightNormals[PointsCount];
  m2::PointF m_leftWidthScalar[PointsCount];
  m2::PointF m_rightWidthScalar[PointsCount];
  bool m_hasLeftJoin[PointsCount];

  LineSegment()
  {
    m_leftWidthScalar[StartPoint] = m_leftWidthScalar[EndPoint] = m2::PointF(1.0f, 0.0f);
    m_rightWidthScalar[StartPoint] = m_rightWidthScalar[EndPoint] = m2::PointF(1.0f, 0.0f);
    m_hasLeftJoin[StartPoint] = m_hasLeftJoin[EndPoint] = true;
  }
};

void UpdateNormalBetweenSegments(LineSegment * segment1, LineSegment * segment2)
{
  ASSERT(segment1 != nullptr, ());
  ASSERT(segment2 != nullptr, ());

  float const dotProduct = m2::DotProduct(segment1->m_leftNormals[EndPoint],
                                          segment2->m_leftNormals[StartPoint]);
  float const absDotProduct = fabs(dotProduct);
  float const eps = 1e-5;

  if (fabs(absDotProduct - 1.0f) < eps)
  {
    // change nothing
    return;
  }

  float const crossProduct = m2::CrossProduct(segment1->m_tangent, segment2->m_tangent);
  if (crossProduct < 0)
  {
    segment1->m_hasLeftJoin[EndPoint] = true;
    segment2->m_hasLeftJoin[StartPoint] = true;

    // change right-side normals
    m2::PointF averageNormal = (segment1->m_rightNormals[EndPoint] + segment2->m_rightNormals[StartPoint]).Normalize();
    segment1->m_rightNormals[EndPoint] = averageNormal;
    segment2->m_rightNormals[StartPoint] = averageNormal;

    float const cosAngle = m2::DotProduct(segment1->m_tangent, averageNormal);
    segment1->m_rightWidthScalar[EndPoint].x = 1.0f / sqrt(1.0f - cosAngle * cosAngle);
    segment1->m_rightWidthScalar[EndPoint].y = segment1->m_rightWidthScalar[EndPoint].x * cosAngle;
    segment2->m_rightWidthScalar[StartPoint] = segment1->m_rightWidthScalar[EndPoint];
  }
  else
  {
    segment1->m_hasLeftJoin[EndPoint] = false;
    segment2->m_hasLeftJoin[StartPoint] = false;

    // change left-side normals
    m2::PointF averageNormal = (segment1->m_leftNormals[EndPoint] + segment2->m_leftNormals[StartPoint]).Normalize();
    segment1->m_leftNormals[EndPoint] = averageNormal;
    segment2->m_leftNormals[StartPoint] = averageNormal;

    float const cosAngle = m2::DotProduct(segment1->m_tangent, averageNormal);
    segment1->m_leftWidthScalar[EndPoint].x = 1.0f / sqrt(1.0f - cosAngle * cosAngle);
    segment1->m_leftWidthScalar[EndPoint].y = segment1->m_leftWidthScalar[EndPoint].x * cosAngle;
    segment2->m_leftWidthScalar[StartPoint] = segment1->m_leftWidthScalar[EndPoint];
  }
}

void CalculateTangentAndNormals(m2::PointF const & pt0, m2::PointF const & pt1,
                                m2::PointF & tangent, m2::PointF & leftNormal,
                                m2::PointF & rightNormal)
{
  tangent = (pt1 - pt0).Normalize();
  leftNormal = m2::PointF(-tangent.y, tangent.x);
  rightNormal = -leftNormal;
}

void ConstructLineSegments(vector<m2::PointD> const & path, vector<LineSegment> & segments)
{
  ASSERT_LESS(1, path.size(), ());

  float const eps = 1e-5;

  m2::PointD prevPoint = path[0];
  for (size_t i = 1; i < path.size(); ++i)
  {
    // filter the same points
    if (prevPoint.EqualDxDy(path[i], eps))
      continue;

    LineSegment segment;

    segment.m_points[StartPoint] = m2::PointF(prevPoint.x, prevPoint.y);
    segment.m_points[EndPoint] = m2::PointF(path[i].x, path[i].y);
    CalculateTangentAndNormals(segment.m_points[StartPoint], segment.m_points[EndPoint], segment.m_tangent,
                               segment.m_leftBaseNormal, segment.m_rightBaseNormal);

    segment.m_leftNormals[StartPoint] = segment.m_leftNormals[EndPoint] = segment.m_leftBaseNormal;
    segment.m_rightNormals[StartPoint] = segment.m_rightNormals[EndPoint] = segment.m_rightBaseNormal;

    prevPoint = path[i];

    segments.push_back(segment);
  }
}

void UpdateNormals(LineSegment * segment, LineSegment * prevSegment, LineSegment * nextSegment)
{
  ASSERT(segment != nullptr, ());

  if (prevSegment != nullptr)
    UpdateNormalBetweenSegments(prevSegment, segment);

  if (nextSegment != nullptr)
    UpdateNormalBetweenSegments(segment, nextSegment);
}

void GenerateJoinNormals(m2::PointF const & normal1, m2::PointF const & normal2,
                         bool isLeft, vector<m2::PointF> & normals)
{
  float const eps = 1e-5;
  float const dotProduct = m2::DotProduct(normal1, normal2);
  if (fabs(dotProduct - 1.0f) < eps)
    return;

  float const segmentAngle = math::pi / 8.0;
  float const fullAngle = acos(dotProduct);
  int segmentsCount = max(static_cast<int>(fullAngle / segmentAngle), 1);

  float const angle = fullAngle / segmentsCount * (isLeft ? -1.0 : 1.0);
  m2::PointF const startNormal = normal1.Normalize();

  for (int i = 0; i < segmentsCount; i++)
  {
    m2::PointF n1 = m2::Rotate(startNormal, i * angle);
    m2::PointF n2 = m2::Rotate(startNormal, (i + 1) * angle);

    normals.push_back(m2::PointF::Zero());
    normals.push_back(isLeft ? n1 : n2);
    normals.push_back(isLeft ? n2 : n1);
  }
}

void GenerateCapNormals(m2::PointF const & normal, bool isStart, vector<m2::PointF> & normals)
{
  int const segmentsCount = 8;
  float const segmentSize = static_cast<float>(math::pi) / segmentsCount * (isStart ? -1.0 : 1.0);
  m2::PointF const startNormal = normal.Normalize();

  for (int i = 0; i < segmentsCount; i++)
  {
    m2::PointF n1 = m2::Rotate(startNormal, i * segmentSize);
    m2::PointF n2 = m2::Rotate(startNormal, (i + 1) * segmentSize);

    normals.push_back(m2::PointF::Zero());
    normals.push_back(isStart ? n1 : n2);
    normals.push_back(isStart ? n2 : n1);
  }
}

m2::PointF GetNormal(LineSegment const & segment, bool isLeft, ENormalType normalType)
{
  if (normalType == BaseNormal)
    return isLeft ? segment.m_leftBaseNormal : segment.m_rightBaseNormal;

  int const index = (normalType == StartNormal) ? StartPoint : EndPoint;
  return isLeft ? segment.m_leftNormals[index] * segment.m_leftWidthScalar[index].x:
                  segment.m_rightNormals[index] * segment.m_rightWidthScalar[index].x;
}

double GenerateGeometry(vector<m2::PointD> const & points, bool isRoute, double lengthScalar,
                        vector<RouteJoinBounds> & joinBounds, TGeometryBuffer & geometry,
                        TIndexBuffer & indices, unsigned short & indexCounter)
{
  float depth = 0.0f;

  auto const generateTriangles = [&geometry, &indices, &indexCounter, &depth](m2::PointF const & pivot,
                                 vector<m2::PointF> const & normals, m2::PointF const & length, bool isLeft)
  {
    float const eps = 1e-5;
    size_t const trianglesCount = normals.size() / 3;
    float const side = isLeft ? kLeftSide : kRightSide;
    for (int j = 0; j < trianglesCount; j++)
    {
      float const lenZ1 = normals[3 * j].Length() < eps ? kCenter : side;
      float const lenZ2 = normals[3 * j + 1].Length() < eps ? kCenter : side;
      float const lenZ3 = normals[3 * j + 2].Length() < eps ? kCenter : side;

      geometry.push_back(TRV(pivot, depth, normals[3 * j], length, lenZ1));
      geometry.push_back(TRV(pivot, depth, normals[3 * j + 1], length, lenZ2));
      geometry.push_back(TRV(pivot, depth, normals[3 * j + 2], length, lenZ3));

      indices.push_back(indexCounter);
      indices.push_back(indexCounter + 1);
      indices.push_back(indexCounter + 2);
      indexCounter += 3;
    }
  };

  auto const generateIndices = [&indices, &indexCounter]()
  {
    indices.push_back(indexCounter);
    indices.push_back(indexCounter + 1);
    indices.push_back(indexCounter + 3);
    indices.push_back(indexCounter + 3);
    indices.push_back(indexCounter + 2);
    indices.push_back(indexCounter);
    indexCounter += 4;
  };

  // constuct segments
  vector<LineSegment> segments;
  segments.reserve(points.size() - 1);
  ConstructLineSegments(points, segments);

  // build geometry
  float length = 0;
  vector<m2::PointF> normals;
  normals.reserve(24);
  for (size_t i = 0; i < segments.size(); i++)
  {
    UpdateNormals(&segments[i], (i > 0) ? &segments[i - 1] : nullptr,
                 (i < segments.size() - 1) ? &segments[i + 1] : nullptr);

    // generate main geometry
    m2::PointF const startPivot = segments[i].m_points[StartPoint];
    m2::PointF const endPivot = segments[i].m_points[EndPoint];

    float const endLength = length + (endPivot - startPivot).Length();

    m2::PointF const leftNormalStart = GetNormal(segments[i], true /* isLeft */, StartNormal);
    m2::PointF const rightNormalStart = GetNormal(segments[i], false /* isLeft */, StartNormal);
    m2::PointF const leftNormalEnd = GetNormal(segments[i], true /* isLeft */, EndNormal);
    m2::PointF const rightNormalEnd = GetNormal(segments[i], false /* isLeft */, EndNormal);

    float projLeftStart = 0.0;
    float projLeftEnd = 0.0;
    float projRightStart = 0.0;
    float projRightEnd = 0.0;
    float scaledLength = length / lengthScalar;
    float scaledEndLength = endLength / lengthScalar;
    if (isRoute)
    {
      projLeftStart = -segments[i].m_leftWidthScalar[StartPoint].y / lengthScalar;
      projLeftEnd = segments[i].m_leftWidthScalar[EndPoint].y / lengthScalar;
      projRightStart = -segments[i].m_rightWidthScalar[StartPoint].y / lengthScalar;
      projRightEnd = segments[i].m_rightWidthScalar[EndPoint].y / lengthScalar;
    }
    else
    {
      float const arrowTailEndCoord = arrowTailSize;
      float const arrowBodyEndCoord = arrowTailEndCoord + arrowTailSize;
      float const arrowHeadStartCoord = 1.0 - arrowHeadSize;
      if (i == 0)
      {
        scaledLength = 0.0f;
        scaledEndLength = arrowTailEndCoord;
      }
      else if (i == segments.size() - 1)
      {
        scaledLength = arrowHeadStartCoord;
        scaledEndLength = 1.0f;
      }
      else
      {
        scaledLength = arrowTailEndCoord;
        scaledEndLength = arrowBodyEndCoord;
      }
    }

    geometry.push_back(TRV(startPivot, depth, m2::PointF::Zero(), m2::PointF(scaledLength, 0), kCenter));
    geometry.push_back(TRV(startPivot, depth, leftNormalStart, m2::PointF(scaledLength, projLeftStart), kLeftSide));
    geometry.push_back(TRV(endPivot, depth, m2::PointF::Zero(), m2::PointF(scaledEndLength, 0), kCenter));
    geometry.push_back(TRV(endPivot, depth, leftNormalEnd, m2::PointF(scaledEndLength, projLeftEnd), kLeftSide));
    generateIndices();

    geometry.push_back(TRV(startPivot, depth, rightNormalStart, m2::PointF(scaledLength, projRightStart), kRightSide));
    geometry.push_back(TRV(startPivot, depth, m2::PointF::Zero(), m2::PointF(scaledLength, 0), kCenter));
    geometry.push_back(TRV(endPivot, depth, rightNormalEnd, m2::PointF(scaledEndLength, projRightEnd), kRightSide));
    geometry.push_back(TRV(endPivot, depth, m2::PointF::Zero(), m2::PointF(scaledEndLength, 0), kCenter));
    generateIndices();

    // generate joins
    if (i < segments.size() - 1)
    {
      normals.clear();
      m2::PointF n1 = segments[i].m_hasLeftJoin[EndPoint] ? segments[i].m_leftNormals[EndPoint] :
                                                            segments[i].m_rightNormals[EndPoint];
      m2::PointF n2 = segments[i + 1].m_hasLeftJoin[StartPoint] ? segments[i + 1].m_leftNormals[StartPoint] :
                                                                  segments[i + 1].m_rightNormals[StartPoint];
      GenerateJoinNormals(n1, n2, segments[i].m_hasLeftJoin[EndPoint], normals);
      generateTriangles(endPivot, normals, m2::PointF(scaledEndLength, 0), segments[i].m_hasLeftJoin[EndPoint]);
    }

    // generate caps
    if (isRoute && i == 0)
    {
      normals.clear();
      GenerateCapNormals(segments[i].m_rightNormals[StartPoint], true /* isStart */, normals);
      generateTriangles(startPivot, normals, m2::PointF(scaledLength, 0), true);
    }

    if (isRoute && i == segments.size() - 1)
    {
      normals.clear();
      GenerateCapNormals(segments[i].m_rightNormals[EndPoint], false /* isStart */, normals);
      generateTriangles(endPivot, normals, m2::PointF(scaledEndLength, 0), true);
    }

    length = endLength;
  }

  // calculate joins bounds
  if (isRoute)
  {
    float const eps = 1e-5;
    double len = 0;
    for (size_t i = 0; i + 1 < segments.size(); i++)
    {
      len += (segments[i].m_points[EndPoint] - segments[i].m_points[StartPoint]).Length();

      RouteJoinBounds bounds;
      bounds.m_start = min(segments[i].m_leftWidthScalar[EndPoint].y,
                           segments[i].m_rightWidthScalar[EndPoint].y);
      bounds.m_end = max(-segments[i + 1].m_leftWidthScalar[StartPoint].y,
                         -segments[i + 1].m_rightWidthScalar[StartPoint].y);

      if (fabs(bounds.m_end - bounds.m_start) < eps)
        continue;

      bounds.m_offset = len;
      joinBounds.push_back(bounds);
    }
  }

  return length;
}

}

void RouteShape::PrepareGeometry(m2::PolylineD const & polyline, RouteData & output)
{
  vector<m2::PointD> const & path = polyline.GetPoints();
  ASSERT_LESS(1, path.size(), ());

  output.m_joinsBounds.clear();
  output.m_geometry.clear();
  output.m_indices.clear();

  unsigned short indexCounter = 0;
  output.m_length = GenerateGeometry(path, true /* isRoute */, 1.0, output.m_joinsBounds,
                                     output.m_geometry, output.m_indices, indexCounter);
}

void RouteShape::PrepareArrowGeometry(vector<m2::PointD> const & points,
                                      double start, double end, ArrowsBuffer & output)
{
  vector<RouteJoinBounds> bounds;
  GenerateGeometry(points, false /* isRoute */, end - start, bounds,
                   output.m_geometry, output.m_indices, output.m_indexCounter);
}

} // namespace rg
