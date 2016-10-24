/*
 * Original work Copyright (c) 2006-2009 Erin Catto http://www.box2d.org
 * Modified work Copyright (c) 2016 Louis Langholtz https://github.com/louis-langholtz/Box2D
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 * 1. The origin of this software must not be misrepresented; you must not
 * claim that you wrote the original software. If you use this software
 * in a product, an acknowledgment in the product documentation would be
 * appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 * misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 */

#include <Box2D/Collision/Collision.h>
#include <Box2D/Collision/CollideShapes.hpp>
#include <Box2D/Collision/Shapes/CircleShape.h>
#include <Box2D/Collision/Shapes/PolygonShape.h>
#include <Box2D/Collision/Shapes/EdgeShape.h>

using namespace box2d;

Manifold box2d::CollideShapes(const CircleShape& shapeA, const Transformation& xfA,
							  const CircleShape& shapeB, const Transformation& xfB)
{
	const auto pA = Transform(shapeA.GetPosition(), xfA);
	const auto pB = Transform(shapeB.GetPosition(), xfB);
	const auto totalRadius = shapeA.GetRadius() + shapeB.GetRadius();
	
	if (LengthSquared(pB - pA) > Square(totalRadius))
	{
		return Manifold{};
	}
	return Manifold::GetForCircles(shapeA.GetPosition(), Manifold::Point{shapeB.GetPosition()});
}

Manifold box2d::CollideShapes(const PolygonShape& shapeA, const Transformation& xfA,
							  const CircleShape& shapeB, const Transformation& xfB)
{
	// Computes the center of the circle in the frame of the polygon.
	const auto cLocal = InverseTransform(Transform(shapeB.GetPosition(), xfB), xfA); ///< Center of the circle in the frame of the polygon.
	
	// Find the min separating edge.
	const auto totalRadius = GetRadius(shapeA) + GetRadius(shapeB);
	const auto vertexCount = shapeA.GetVertexCount();
	auto normalIndex = decltype(vertexCount){0};
	auto maxSeparation = -MaxFloat;
	
	for (auto i = decltype(vertexCount){0}; i < vertexCount; ++i)
	{
		const auto s = Dot(shapeA.GetNormal(i), cLocal - shapeA.GetVertex(i));
		if (s > totalRadius)
		{
			// Early out.
			return Manifold{};
		}
		if (maxSeparation < s)
		{
			maxSeparation = s;
			normalIndex = i;
		}
	}
	
	// Vertices that subtend the incident face.
	const auto vertIndex1 = normalIndex;
	const auto vertIndex2 = static_cast<decltype(vertIndex1)>((vertIndex1 + 1) % vertexCount);
	const auto v1 = shapeA.GetVertex(vertIndex1);
	const auto v2 = shapeA.GetVertex(vertIndex2);
	
	// If the center is inside the polygon ...
	if ((maxSeparation < 0) || almost_equal(maxSeparation, 0))
	{
		return Manifold::GetForFaceA(shapeA.GetNormal(normalIndex), (v1 + v2) / 2, Manifold::Point{shapeB.GetPosition()});
	}
	
	// Compute barycentric coordinates
	
	if (Dot(cLocal - v1, v2 - v1) <= float_t{0})
	{
		if (LengthSquared(cLocal - v1) > Square(totalRadius))
		{
			return Manifold{};
		}
		return Manifold::GetForFaceA(GetUnitVector(cLocal - v1), v1, Manifold::Point{shapeB.GetPosition()});
	}
	
	if (Dot(cLocal - v2, v1 - v2) <= float_t{0})
	{
		if (LengthSquared(cLocal - v2) > Square(totalRadius))
		{
			return Manifold{};
		}
		return Manifold::GetForFaceA(GetUnitVector(cLocal - v2), v2, Manifold::Point{shapeB.GetPosition()});
	}
	
	const auto faceCenter = (v1 + v2) / 2;
	if (Dot(cLocal - faceCenter, shapeA.GetNormal(vertIndex1)) > totalRadius)
	{
		return Manifold{};
	}
	return Manifold::GetForFaceA(shapeA.GetNormal(vertIndex1), faceCenter, Manifold::Point{shapeB.GetPosition()});
}

Manifold box2d::CollideShapes(const EdgeShape& shapeA, const Transformation& xfA,
							  const CircleShape& shapeB, const Transformation& xfB)
{
	/*
	 * Determine the collision manifold between the edge and the circle. This accounts for
	 * edge connectivity.
	 *
	 * To do this, this code treats the edge like an end-rounded strait-line drawn by an air
	 * brush having the radius that GetRadius(shapeA) returns. As such, the collision is
	 * categorized as either a collision with the first end-point, the second end-point, or the
	 * conceptual rectangle between the end-points.
	 *
	 * Definitions:
	 *   "-" Is an edge parallel to the horizon bounded by vertex A on the left and vertex B
	 *       on the right.
	 *   "|" Is a line perpendicular to the edge.
	 *   "." Is the center of a circle whose position is called Q.
	 *
	 * Then:
	 *   When Q is anywhere to the left of A (as in ".|-"), it's said to be in region A.
	 *   When Q is anywhere to the right of B (as in "-|."), it's said to be in region B.
	 *   Otheriwse (when Q is between A and B), Q is said to be in region AB.
	 */
	
	// Compute circle in frame of edge
	const auto Q = InverseTransform(Transform(shapeB.GetPosition(), xfB), xfA); ///< Circle's position in frame of edge.
	
	const auto A = shapeA.GetVertex1(); ///< Edge shape's vertex 1.
	const auto B = shapeA.GetVertex2(); ///< Edge shape's vertex 2.
	const auto e = B - A; ///< Edge shape's primary edge.
	
	// Barycentric coordinates
	
	const auto totalRadius = GetRadius(shapeA) + GetRadius(shapeB);
	
	// Check if circle's center is relatively left of first vertex of edge - this is "Region A"
	const auto v = Dot(e, Q - A);
	if (v <= 0)
	{
		const auto P = A; ///< Point of relavance (is A).
		if (LengthSquared(Q - P) > Square(totalRadius))
		{
			return Manifold{};
		}
		
		// Is there an edge connected to A?
		if (shapeA.HasVertex0())
		{
			const auto A1 = shapeA.GetVertex0();
			const auto B1 = A;
			const auto e1 = B1 - A1;
			const auto u1 = Dot(e1, B1 - Q);
			
			// Is the circle in Region AB of the previous edge?
			if (u1 > 0)
			{
				return Manifold{};
			}
		}
		
		const auto cf = ContactFeature{ContactFeature::e_vertex, 0, ContactFeature::e_vertex, 0};
		return Manifold::GetForCircles(P, Manifold::Point{shapeB.GetPosition(), cf});
	}
	
	// Check if circle's center is relatively right of second vertex of edge - this is "Region B"
	const auto u = Dot(e, B - Q);
	if (u <= 0)
	{
		const auto P = B; ///< Point of relavance (is B).
		if (LengthSquared(Q - P) > Square(totalRadius))
		{
			return Manifold{};
		}
		
		// Is there an edge connected to B?
		if (shapeA.HasVertex3())
		{
			const auto B2 = shapeA.GetVertex3();
			const auto A2 = B;
			const auto e2 = B2 - A2;
			const auto v2 = Dot(e2, Q - A2);
			
			// Is the circle in Region AB of the next edge?
			if (v2 > 0)
			{
				return Manifold{};
			}
		}
		
		const auto cf = ContactFeature{ContactFeature::e_vertex, 1, ContactFeature::e_vertex, 0};
		return Manifold::GetForCircles(P, Manifold::Point{shapeB.GetPosition(), cf});
	}
	
	// Region AB
	const auto eLenSquared = LengthSquared(e);
	assert(eLenSquared > 0);
	const auto P = (u * A + v * B) * (float_t{1} / eLenSquared);
	const auto d = Q - P;
	
	if (LengthSquared(d) > Square(totalRadius))
	{
		return Manifold{};
	}
	
	const auto n = [=]() {
		const auto e_perp = GetRevPerpendicular(e);
		return (Dot(e_perp, Q - A) < 0)? -e_perp: e_perp;
	}();
	
	const auto cf = ContactFeature{ContactFeature::e_face, 0, ContactFeature::e_vertex, 0};
	return Manifold::GetForFaceA(GetUnitVector(n), A, Manifold::Point{shapeB.GetPosition(), cf});
}

// This structure is used to keep track of the best separating axis.
struct EPAxis
{
	using index_t = PolygonShape::vertex_count_t;
	
	enum Type: index_t
	{
		/// Unknown type.
		/// @detail Index and separation are invalid.
		e_unknown,
		
		/// Edge type.
		/// @detail Index refers to the edge shape.
		e_edge,

		/// Polygon type.
		/// @detail Index refers to the polygon shape.
		e_polygon
	};
	
	static constexpr index_t InvalidIndex = static_cast<index_t>(-1);
	static constexpr float_t InvalidSeparation = GetInvalid<float_t>();
	
	Type type = e_unknown;
	index_t index = InvalidIndex;
	float_t separation = InvalidSeparation; ///< Separation.
};

// Reference face used for clipping
struct ReferenceFace
{
	using index_t = PolygonShape::vertex_count_t;
	
	index_t i1, i2;
	
	Vec2 v1, v2;
	
	Vec2 normal;
	
	Vec2 sideNormal1;
	float_t sideOffset1;
	
	Vec2 sideNormal2;
	float_t sideOffset2;
};

class EdgeInfo
{
public:
	EdgeInfo() = default;
	
	EdgeInfo(const EdgeShape& edge, const Vec2& centroid);
	
	Vec2 GetVertex1() const noexcept { return m_vertex1; }
	Vec2 GetVertex2() const noexcept { return m_vertex2; }
	Vec2 GetEdge1() const noexcept { return m_edge1; }
	Vec2 GetNormal1() const noexcept { return m_normal1; }
	
	bool IsFront() const noexcept { return m_front; }
	Vec2 GetNormal() const noexcept { return m_normal; }
	Vec2 GetLowerLimit() const noexcept { return m_lowerLimit; }
	Vec2 GetUpperLimit() const noexcept { return m_upperLimit; }
	
private:
	Vec2 m_vertex1;
	Vec2 m_vertex2;
	Vec2 m_edge1;
	Vec2 m_normal1;
	
	bool m_front;
	Vec2 m_normal;
	Vec2 m_lowerLimit, m_upperLimit;
};

inline EdgeInfo::EdgeInfo(const EdgeShape& edge, const Vec2& centroid):
	m_vertex1(edge.GetVertex1()),
	m_vertex2(edge.GetVertex2()),
	m_edge1(GetUnitVector(m_vertex2 - m_vertex1)),
	m_normal1(GetFwdPerpendicular(m_edge1))
{
	const auto hasVertex0 = edge.HasVertex0();
	const auto hasVertex3 = edge.HasVertex3();
	
	const auto offset1 = Dot(m_normal1, centroid - m_vertex1);
	
	// Determine front or back collision. Determine collision normal limits.
	if (hasVertex0 && hasVertex3)
	{
		const auto vertex0 = edge.GetVertex0();
		const auto edge0 = GetUnitVector(m_vertex1 - vertex0);
		const auto normal0 = GetFwdPerpendicular(edge0);
		const auto convex1 = Cross(edge0, m_edge1) >= float_t{0};
		const auto offset0 = Dot(normal0, centroid - vertex0);
		
		const auto vertex3 = edge.GetVertex3();
		const auto edge2 = GetUnitVector(vertex3 - m_vertex2);
		const auto normal2 = GetFwdPerpendicular(edge2);
		const auto convex2 = Cross(m_edge1, edge2) > float_t{0};
		const auto offset2 = Dot(normal2, centroid - m_vertex2);
		
		if (convex1 && convex2)
		{
			m_front = (offset0 >= 0) || (offset1 >= 0) || (offset2 >= 0);
			if (m_front)
			{
				m_normal = m_normal1;
				m_lowerLimit = normal0;
				m_upperLimit = normal2;
			}
			else
			{
				m_normal = -m_normal1;
				m_lowerLimit = -m_normal1;
				m_upperLimit = -m_normal1;
			}
		}
		else if (convex1)
		{
			m_front = (offset0 >= 0) || ((offset1 >= 0) && (offset2 >= 0));
			if (m_front)
			{
				m_normal = m_normal1;
				m_lowerLimit = normal0;
				m_upperLimit = m_normal1;
			}
			else
			{
				m_normal = -m_normal1;
				m_lowerLimit = -normal2;
				m_upperLimit = -m_normal1;
			}
		}
		else if (convex2)
		{
			m_front = (offset2 >= 0) || ((offset0 >= 0) && (offset1 >= 0));
			if (m_front)
			{
				m_normal = m_normal1;
				m_lowerLimit = m_normal1;
				m_upperLimit = normal2;
			}
			else
			{
				m_normal = -m_normal1;
				m_lowerLimit = -m_normal1;
				m_upperLimit = -normal0;
			}
		}
		else // !convex1 && !convex2
		{
			m_front = (offset0 >= 0) && (offset1 >= 0) && (offset2 >= 0);
			if (m_front)
			{
				m_normal = m_normal1;
				m_lowerLimit = m_normal1;
				m_upperLimit = m_normal1;
			}
			else
			{
				m_normal = -m_normal1;
				m_lowerLimit = -normal2;
				m_upperLimit = -normal0;
			}
		}
	}
	else if (hasVertex0)
	{
		const auto vertex0 = edge.GetVertex0();
		const auto edge0 = GetUnitVector(m_vertex1 - vertex0);
		const auto normal0 = GetFwdPerpendicular(edge0);
		const auto convex1 = Cross(edge0, m_edge1) >= float_t{0};
		const auto offset0 = Dot(normal0, centroid - vertex0);
		
		if (convex1)
		{
			m_front = (offset0 >= 0) || (offset1 >= 0);
			if (m_front)
			{
				m_normal = m_normal1;
				m_lowerLimit = normal0;
				m_upperLimit = -m_normal1;
			}
			else
			{
				m_normal = -m_normal1;
				m_lowerLimit = m_normal1;
				m_upperLimit = -m_normal1;
			}
		}
		else // !convex1
		{
			m_front = (offset0 >= 0) && (offset1 >= 0);
			if (m_front)
			{
				m_normal = m_normal1;
				m_lowerLimit = m_normal1;
				m_upperLimit = -m_normal1;
			}
			else
			{
				m_normal = -m_normal1;
				m_lowerLimit = m_normal1;
				m_upperLimit = -normal0;
			}
		}
	}
	else if (hasVertex3)
	{
		const auto vertex3 = edge.GetVertex3();
		const auto edge2 = GetUnitVector(vertex3 - m_vertex2);
		const auto normal2 = GetFwdPerpendicular(edge2);
		const auto convex2 = Cross(m_edge1, edge2) > float_t{0};
		const auto offset2 = Dot(normal2, centroid - m_vertex2);
		
		if (convex2)
		{
			m_front = (offset1 >= 0) || (offset2 >= 0);
			if (m_front)
			{
				m_normal = m_normal1;
				m_lowerLimit = -m_normal1;
				m_upperLimit = normal2;
			}
			else
			{
				m_normal = -m_normal1;
				m_lowerLimit = -m_normal1;
				m_upperLimit = m_normal1;
			}
		}
		else // !convex2
		{
			m_front = (offset1 >= 0) && (offset2 >= 0);
			if (m_front)
			{
				m_normal = m_normal1;
				m_lowerLimit = -m_normal1;
				m_upperLimit = m_normal1;
			}
			else
			{
				m_normal = -m_normal1;
				m_lowerLimit = -normal2;
				m_upperLimit = m_normal1;
			}
		}		
	}
	else // !hasVertex0 && !hasVertex3
	{
		m_front = offset1 >= 0;
		if (m_front)
		{
			m_normal = m_normal1;
			m_lowerLimit = -m_normal1;
			m_upperLimit = -m_normal1;
		}
		else
		{
			m_normal = -m_normal1;
			m_lowerLimit = m_normal1;
			m_upperLimit = m_normal1;
		}
	}
}

static inline PolygonShape::vertex_count_t GetIndexOfMinimum(const PolygonShape& polygon,
															 const EdgeInfo& edge)
{
	auto bestIndex = PolygonShape::InvalidVertex;
	{
		auto minValue = MaxFloat;
		const auto count = polygon.GetVertexCount();
		for (auto i = decltype(count){0}; i < count; ++i)
		{
			const auto value = Dot(edge.GetNormal(), polygon.GetNormal(i));
			if (minValue > value)
			{
				minValue = value;
				bestIndex = i;
			}
		}
	}
	return bestIndex;
}

static constexpr float_t MaxEPSeparation = PolygonRadius * 2; ///< Maximum separation.

static inline EPAxis ComputeEdgeSeparation(const PolygonShape& polygon, const EdgeInfo& edge)
{
	auto minValue = MaxFloat;
	{
		const auto count = polygon.GetVertexCount();
		for (auto i = decltype(count){0}; i < count; ++i)
		{
			const auto s = Dot(edge.GetNormal(), polygon.GetVertex(i) - edge.GetVertex1());
			minValue = Min(minValue, s);
		}
	}
	return EPAxis{EPAxis::e_edge, static_cast<EPAxis::index_t>(edge.IsFront()? 0: 1), minValue};
}

static inline EPAxis ComputePolygonSeparation(const PolygonShape& polygon, const EdgeInfo& edge)
{
	auto max_s = -MaxFloat;
	auto index = EPAxis::InvalidIndex;
	{
		const auto perp = GetRevPerpendicular(edge.GetNormal());
		const auto count = polygon.GetVertexCount();
		for (auto i = decltype(count){0}; i < count; ++i)
		{
			const auto polygonNormal = -polygon.GetNormal(i);
			const auto polygonVertex = polygon.GetVertex(i);
			const auto s = Min(Dot(polygonNormal, polygonVertex - edge.GetVertex1()),
							   Dot(polygonNormal, polygonVertex - edge.GetVertex2()));
			
			if (s > MaxEPSeparation) // No collision
			{
				return EPAxis{EPAxis::e_polygon, i, s};
			}
			
			// Adjacency
			if (Dot(polygonNormal, perp) >= 0)
			{
				if (Dot(polygonNormal - edge.GetUpperLimit(), edge.GetNormal()) < -AngularSlop)
				{
					continue;
				}
			}
			else
			{
				if (Dot(polygonNormal - edge.GetLowerLimit(), edge.GetNormal()) < -AngularSlop)
				{
					continue;
				}
			}
			
			if (max_s < s)
			{
				max_s = s;
				index = i;
			}
		}
	}
	return EPAxis{EPAxis::e_polygon, index, max_s};
}

Manifold box2d::CollideShapes(const EdgeShape& shapeA, const Transformation& xfA,
							  const PolygonShape& shapeB, const Transformation& xfB)
{
	// Algorithm:
	// 1. Classify v1 and v2
	// 2. Classify polygon centroid as front or back
	// 3. Flip normal if necessary
	// 4. Initialize normal range to [-pi, pi] about face normal
	// 5. Adjust normal range according to adjacent edges
	// 6. Visit each separating axes, only accept axes within the range
	// 7. Return if _any_ axis indicates separation
	// 8. Clip

	const auto xf = MulT(xfA, xfB);
	const auto localShapeB = Transform(shapeB, xf);
	const auto edgeInfo = EdgeInfo{shapeA, localShapeB.GetCentroid()};
	
	const auto edgeAxis = ComputeEdgeSeparation(localShapeB, edgeInfo);
	assert(edgeAxis.type == EPAxis::e_edge);
	
	// If no valid normal can be found then this edge should not collide.
	if (edgeAxis.separation > MaxEPSeparation)
	{
		return Manifold{};
	}
	
	const auto polygonAxis = ComputePolygonSeparation(localShapeB, edgeInfo);
	assert(polygonAxis.type == EPAxis::e_polygon);

	if (polygonAxis.separation > MaxEPSeparation)
	{
		return Manifold{};
	}
	
	// Use hysteresis for jitter reduction.
	constexpr auto k_relativeTol = float_t(0.98);
	constexpr auto k_absoluteTol = LinearSlop / 5; // 0.001
	
	// Now:
	//   (edgeAxis.separation <= MaxSeparation) AND
	//   (polygonAxis.type == EPAxis::e_unknown OR polygonAxis.separation <= MaxSeparation)
	const auto primaryAxis = (polygonAxis.index == EPAxis::InvalidIndex)?
		edgeAxis: (polygonAxis.separation > ((k_relativeTol * edgeAxis.separation) + k_absoluteTol))?
			polygonAxis: edgeAxis;
	
	ClipList incidentEdge;
	ReferenceFace rf;
	switch (primaryAxis.type)
	{
		case EPAxis::e_edge:
		{
			// Search for the polygon normal that is most anti-parallel to the edge normal.
			const auto i1 = GetIndexOfMinimum(localShapeB, edgeInfo);
			const auto i2 = static_cast<decltype(i1)>((i1 + 1) % localShapeB.GetVertexCount());
			
			incidentEdge.add(ClipVertex{
				localShapeB.GetVertex(i1),
				ContactFeature{ContactFeature::e_face, 0, ContactFeature::e_vertex, i1}
			});
			incidentEdge.add(ClipVertex{
				localShapeB.GetVertex(i2),
				ContactFeature{ContactFeature::e_face, 0, ContactFeature::e_vertex, i2}
			});
			
			if (edgeInfo.IsFront())
			{
				rf.i1 = 0;
				rf.i2 = 1;
				rf.v1 = edgeInfo.GetVertex1();
				rf.v2 = edgeInfo.GetVertex2();
				rf.normal = edgeInfo.GetNormal1();
			}
			else
			{
				rf.i1 = 1;
				rf.i2 = 0;
				rf.v1 = edgeInfo.GetVertex2();
				rf.v2 = edgeInfo.GetVertex1();
				rf.normal = -edgeInfo.GetNormal1();
			}
			break;
		}
		case EPAxis::e_polygon:
		{
			incidentEdge.add(ClipVertex{
				edgeInfo.GetVertex1(),
				ContactFeature{ContactFeature::e_vertex, 0, ContactFeature::e_face, primaryAxis.index}
			});
			incidentEdge.add(ClipVertex{
				edgeInfo.GetVertex2(),
				ContactFeature{ContactFeature::e_vertex, 0, ContactFeature::e_face, primaryAxis.index}
			});
			
			rf.i1 = primaryAxis.index;
			rf.i2 = static_cast<decltype(rf.i1)>((rf.i1 + 1) % localShapeB.GetVertexCount());
			rf.v1 = localShapeB.GetVertex(rf.i1);
			rf.v2 = localShapeB.GetVertex(rf.i2);
			rf.normal = localShapeB.GetNormal(rf.i1);
			break;
		}
		default: // never reached
			break;
	}
	
	rf.sideNormal1 = GetFwdPerpendicular(rf.normal);
	rf.sideNormal2 = -rf.sideNormal1;
	rf.sideOffset1 = Dot(rf.sideNormal1, rf.v1);
	rf.sideOffset2 = Dot(rf.sideNormal2, rf.v2);
	
	// Clip incident edge against extruded edge1 side edges.
	const auto clipPoints = ClipSegmentToLine(ClipSegmentToLine(incidentEdge, rf.sideNormal1, rf.sideOffset1, rf.i1),
											   rf.sideNormal2, rf.sideOffset2, rf.i2);
	if (clipPoints.size() != 2)
	{
		return Manifold{};
	}
	
	// Now clipPoints2 contains the clipped points.	
	
	switch (primaryAxis.type)
	{
		case EPAxis::e_edge:
		{
			auto manifold = Manifold::GetForFaceA(rf.normal, rf.v1);
			for (auto i = decltype(clipPoints.size()){0}; i < clipPoints.size(); ++i)
			{
				const auto separation = Dot(rf.normal, clipPoints[i].v - rf.v1);
				if (separation <= MaxEPSeparation)
				{
					manifold.AddPoint(Manifold::Point{InverseTransform(clipPoints[i].v, xf), clipPoints[i].cf});
				}
			}
			return manifold;
		}
		case EPAxis::e_polygon:
		{
			auto manifold = Manifold::GetForFaceB(shapeB.GetNormal(rf.i1), shapeB.GetVertex(rf.i1));
			for (auto i = decltype(clipPoints.size()){0}; i < clipPoints.size(); ++i)
			{
				const auto separation = Dot(rf.normal, clipPoints[i].v - rf.v1);
				if (separation <= MaxEPSeparation)
				{
					manifold.AddPoint(Manifold::Point{clipPoints[i].v, Flip(clipPoints[i].cf)});
				}
			}
			return manifold;
		}
		default: // never reached
			break;
	}
	return Manifold{};
}

struct ShapeSeparation
{
	using index_t = std::remove_const<decltype(MaxShapeVertices)>::type;
	
	index_t index;
	float_t separation;
};

// Find the max separation between shape1 and shape2 using edge normals from shape1.
static ShapeSeparation FindMaxSeparation(const PolygonShape& shape1, const Transformation& xf1,
										 const PolygonShape& shape2, const Transformation& xf2)
{
	auto maxSeparation = -MaxFloat;
	auto index_of_max = PolygonShape::vertex_count_t{0};
	{
		const auto count1 = shape1.GetVertexCount();
		const auto count2 = shape2.GetVertexCount();
		const auto xf = MulT(xf2, xf1);
		
		for (auto i = decltype(count1){0}; i < count1; ++i)
		{
			// Get shape1 normal in frame2.
			const auto n = Rotate(shape1.GetNormal(i), xf.q);
			const auto v1 = Transform(shape1.GetVertex(i), xf);
			
			// Find deepest point for normal i.
			auto min_sij = MaxFloat;
			for (auto j = decltype(count2){0}; j < count2; ++j)
			{
				const auto sij = Dot(n, shape2.GetVertex(j) - v1);
				min_sij = Min(min_sij, sij);
			}
			
			if (maxSeparation < min_sij)
			{
				maxSeparation = min_sij;
				index_of_max = i;
			}
		}
	}
	return ShapeSeparation{index_of_max, maxSeparation};
}

static ClipList FindIncidentEdge(PolygonShape::vertex_count_t index1,
								 const PolygonShape& shape1, const Transformation& xf1,
								 const PolygonShape& shape2, const Transformation& xf2)
{
	ClipList list;

	const auto count1 = shape1.GetVertexCount();
	const auto count2 = shape2.GetVertexCount();
	if ((index1 < count1) && (count2 > 1))
	{
		// Find the incident edge on shape2.
		auto index_of_min_dot = PolygonShape::InvalidVertex;
		{
			// Get the normal of the reference edge in shape2's frame.
			const auto normal1 = InverseRotate(Rotate(shape1.GetNormal(index1), xf1.q), xf2.q);
			
			auto minDot = MaxFloat;
			for (auto i = decltype(count2){0}; i < count2; ++i)
			{
				const auto dot = Dot(normal1, shape2.GetNormal(i));
				if (minDot > dot)
				{
					minDot = dot;
					index_of_min_dot = i;
				}
			}
		}
		
		// Build the clip vertices for the incident edge.
		const auto i1 = index_of_min_dot;
		const auto i2 = static_cast<decltype(i1)>((i1 + 1) % count2);
		
		list.add(ClipVertex{
			Transform(shape2.GetVertex(i1), xf2),
			ContactFeature{ContactFeature::e_face, index1, ContactFeature::e_vertex, i1}
		});
		list.add(ClipVertex{
			Transform(shape2.GetVertex(i2), xf2),
			ContactFeature{ContactFeature::e_face, index1, ContactFeature::e_vertex, i2}
		});
	}

	return list;
}

static inline Manifold GetManifoldFaceA(const PolygonShape& shape1, const Transformation& xf1,
										const PolygonShape& shape2, const Transformation& xf2,
										const PolygonShape::vertex_count_t edgeIndex)
{
	const auto totalRadius = GetRadius(shape1) + GetRadius(shape2);

	const auto iv1 = edgeIndex;
	const auto iv2 = static_cast<decltype(iv1)>((edgeIndex + 1) % (shape1.GetVertexCount()));
	
	const auto shape1_rel_vertex_iv1 = shape1.GetVertex(iv1);
	const auto shape1_rel_vertex_iv2 = shape1.GetVertex(iv2);
	const auto shape1_abs_vertex_iv1 = Transform(shape1_rel_vertex_iv1, xf1);
	const auto shape1_abs_vertex_iv2 = Transform(shape1_rel_vertex_iv2, xf1);

	const auto localTangent = GetUnitVector(shape1_rel_vertex_iv2 - shape1_rel_vertex_iv1);
	const auto tangent = Rotate(localTangent, xf1.q);
	
	// Clip incident edge against extruded edge1 side edges.
	// Side offsets, extended by polytope skin thickness.
	const auto clipPoints = [&]() {
		const auto incidentEdge = FindIncidentEdge(edgeIndex, shape1, xf1, shape2, xf2);
		const auto points = ClipSegmentToLine(incidentEdge, -tangent, -Dot(tangent, shape1_abs_vertex_iv1) + totalRadius, iv1);
		return ClipSegmentToLine(points, tangent, Dot(tangent, shape1_abs_vertex_iv2) + totalRadius, iv2);
	}();
	if (clipPoints.size() == 2)
	{
		const auto planePoint = (shape1_rel_vertex_iv1 + shape1_rel_vertex_iv2) / float_t(2);
		auto manifold = Manifold::GetForFaceA(GetFwdPerpendicular(localTangent), planePoint);
	
		// The normal points from 1 to 2
		const auto normal = GetFwdPerpendicular(tangent);
		
		const auto frontOffset = Dot(normal, shape1_abs_vertex_iv1); ///< Face offset.
		for (auto&& cp: clipPoints)
		{
			if ((Dot(normal, cp.v) - frontOffset) <= totalRadius)
			{
				manifold.AddPoint(Manifold::Point{InverseTransform(cp.v, xf2), cp.cf});
			}
		}		
		return manifold;
	}
	return Manifold{};	
}

static inline Manifold GetManifoldFaceB(const PolygonShape& shape1, const Transformation& xf1,
										const PolygonShape& shape2, const Transformation& xf2,
										const PolygonShape::vertex_count_t edgeIndex)
{
	const auto totalRadius = GetRadius(shape1) + GetRadius(shape2);
	
	const auto iv1 = edgeIndex;
	const auto iv2 = static_cast<decltype(iv1)>((edgeIndex + 1) % (shape1.GetVertexCount()));
	
	const auto shape1_rel_vertex_iv1 = shape1.GetVertex(iv1);
	const auto shape1_rel_vertex_iv2 = shape1.GetVertex(iv2);
	const auto shape1_abs_vertex_iv1 = Transform(shape1_rel_vertex_iv1, xf1);
	const auto shape1_abs_vertex_iv2 = Transform(shape1_rel_vertex_iv2, xf1);

	const auto localTangent = GetUnitVector(shape1_rel_vertex_iv2 - shape1_rel_vertex_iv1);
	const auto tangent = Rotate(localTangent, xf1.q);
	
	// Clip incident edge against extruded edge1 side edges.
	// Side offsets, extended by polytope skin thickness.
	const auto clipPoints = [&]() {
		const auto incidentEdge = FindIncidentEdge(edgeIndex, shape1, xf1, shape2, xf2);
		const auto points = ClipSegmentToLine(incidentEdge, -tangent, -Dot(tangent, shape1_abs_vertex_iv1) + totalRadius, iv1);
		return ClipSegmentToLine(points, tangent, Dot(tangent, shape1_abs_vertex_iv2) + totalRadius, iv2);
	}();
	if (clipPoints.size() == 2)
	{
		const auto planePoint = (shape1_rel_vertex_iv1 + shape1_rel_vertex_iv2) / float_t(2);
		auto manifold = Manifold::GetForFaceB(GetFwdPerpendicular(localTangent), planePoint);
		
		// The normal points from 1 to 2
		const auto normal = GetFwdPerpendicular(tangent);

		const auto frontOffset = Dot(normal, shape1_abs_vertex_iv1); ///< Face offset.
		for (auto&& cp: clipPoints)
		{
			if ((Dot(normal, cp.v) - frontOffset) <= totalRadius)
			{
				manifold.AddPoint(Manifold::Point{InverseTransform(cp.v, xf2), Flip(cp.cf)});
			}
		}
		return manifold;	
	}
	return Manifold{};
}

Manifold box2d::CollideShapes(const PolygonShape& shapeA, const Transformation& xfA,
							  const PolygonShape& shapeB, const Transformation& xfB)
{
	// Find edge normal of max separation on A - return if separating axis is found
	// Find edge normal of max separation on B - return if separation axis is found
	// Choose reference edge as min(minA, minB)
	// Find incident edge
	// Clip

	const auto totalRadius = GetRadius(shapeA) + GetRadius(shapeB);
	
	const auto edgeSepA = FindMaxSeparation(shapeA, xfA, shapeB, xfB);
	if (edgeSepA.separation > totalRadius)
	{
		return Manifold{};
	}
	
	const auto edgeSepB = FindMaxSeparation(shapeB, xfB, shapeA, xfA);
	if (edgeSepB.separation > totalRadius)
	{
		return Manifold{};
	}
	
	constexpr auto k_tol = BOX2D_MAGIC(LinearSlop / 10);
	if (edgeSepB.separation > (edgeSepA.separation + k_tol))
	{
		return GetManifoldFaceB(shapeB, xfB, shapeA, xfA, static_cast<PolygonShape::vertex_count_t>(edgeSepB.index));
	}
	return GetManifoldFaceA(shapeA, xfA, shapeB, xfB, static_cast<PolygonShape::vertex_count_t>(edgeSepA.index));
}