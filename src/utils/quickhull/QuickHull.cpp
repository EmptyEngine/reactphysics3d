/********************************************************************************
* ReactPhysics3D physics library, http://www.reactphysics3d.com                 *
* Copyright (c) 2010-2022 Daniel Chappuis                                       *
*********************************************************************************
*                                                                               *
* This software is provided 'as-is', without any express or implied warranty.   *
* In no event will the authors be held liable for any damages arising from the  *
* use of this software.                                                         *
*                                                                               *
* Permission is granted to anyone to use this software for any purpose,         *
* including commercial applications, and to alter it and redistribute it        *
* freely, subject to the following restrictions:                                *
*                                                                               *
* 1. The origin of this software must not be misrepresented; you must not claim *
*    that you wrote the original software. If you use this software in a        *
*    product, an acknowledgment in the product documentation would be           *
*    appreciated but is not required.                                           *
*                                                                               *
* 2. Altered source versions must be plainly marked as such, and must not be    *
*    misrepresented as being the original software.                             *
*                                                                               *
* 3. This notice may not be removed or altered from any source distribution.    *
*                                                                               *
********************************************************************************/

// Libraries
#include <reactphysics3d/utils/quickhull/QuickHull.h>
#include <reactphysics3d/collision/ConvexMesh.h>
#include <reactphysics3d/collision/PolygonVertexArray.h>
#include <reactphysics3d/collision/VertexArray.h>
#include <reactphysics3d/containers/Map.h>
#include <reactphysics3d/containers/Array.h>
#include <reactphysics3d/containers/Stack.h>
#include <reactphysics3d/containers/Set.h>
#include <reactphysics3d/utils/Error.h>
#include <iostream>

// Namespace
using namespace reactphysics3d;

// Compute the convex hull of a set of points and return the resulting convex mesh
bool QuickHull::computeConvexHull(const VertexArray& vertexArray, PolygonVertexArray& outPolygonVertexArray,
                                  Array<float>& outVertices, Array<unsigned int>& outIndices,
                                  Array<PolygonVertexArray::PolygonFace>& outFaces, MemoryAllocator& allocator,
                                  std::vector<Error>& errors) {

    bool isValid = true;

    // Extract the points from the array
    Array<Vector3> points(allocator);
    extractPoints(vertexArray, points);

    // If there are less than four vertices in the vertex array
    if (points.size() < 4) {

       errors.push_back(Error("The VertexArray must contain at least 4 vertices to create a convex mesh"));
       return false;
    }

    Array<uint32> orphanPointsIndices(allocator, points.size());
    decimal maxAbsX = 0;
    decimal maxAbsY = 0;
    decimal maxAbsZ = 0;
    for (uint32 i=0 ; i < points.size(); i++) {
        orphanPointsIndices.add(i);

        decimal absX = std::abs(points[i].x);
        decimal absY = std::abs(points[i].y);
        decimal absZ = std::abs(points[i].z);

        if (absX > maxAbsX) {
            maxAbsX = absX;
        }
        if (absY > maxAbsY) {
            maxAbsY = absY;
        }
        if (absZ > maxAbsZ) {
            maxAbsZ = absZ;
        }
    }

    // Compute the 'epsilon' value for this set of points
    const decimal epsilon = 3 * (maxAbsX + maxAbsY + maxAbsZ) * MACHINE_EPSILON;

    QHHalfEdgeStructure convexHull(allocator);

    Array<QHHalfEdgeStructure::Face*> initialFaces(allocator);

    // Compute the initial convex hull
    isValid &= computeInitialHull(points, convexHull, initialFaces, orphanPointsIndices, allocator, errors);
    if (!isValid) {
        return false;
    }

    assert(convexHull.getNbVertices() == 4);
    assert(convexHull.getNbFaces() == 4);

    std::cout << " Initial hull: " << std::endl << convexHull.to_string() << std::endl;

    assert(convexHull.getNbVertices() == 4);

    // Associate all the remaining points with the closest faces of the initial hull
    associateOrphanPointsToNewFaces(orphanPointsIndices, initialFaces, points, epsilon);

    // Get The next vertex candidate
    uint32 nextVertexIndex;
    QHHalfEdgeStructure::Face* nextFace;
    findNextVertexCandidate(points, nextVertexIndex, convexHull, nextFace, epsilon);
    while (nextVertexIndex != INVALID_VERTEX_INDEX) {

        assert(nextFace != nullptr);

        // TODO : REMOVE ALL CONSOLE OUTPUT FROM QUICKHULL
        std::cout << "Adding vertex " << nextVertexIndex << " to the hull" << std::endl;

        // Add the vertex to the hull
        addVertexToHull(nextVertexIndex, nextFace, points, convexHull, epsilon, allocator);

        std::cout << " New hull: " << std::endl << convexHull.to_string() << std::endl;

        // Get the next vertex candidate
        findNextVertexCandidate(points, nextVertexIndex, convexHull, nextFace, epsilon);

        assert(convexHull.isValid());
    }

    // Compute the final PolygonVertexArray with the resulting convex hull mesh
    computeFinalPolygonVertexArray(convexHull, points, outPolygonVertexArray, outVertices, outIndices, outFaces, allocator);

    assert(convexHull.isValid());

    return isValid;
}

// Compute the final PolygonVertexArray from the convex hull half-edge structure
void QuickHull::computeFinalPolygonVertexArray(const QHHalfEdgeStructure& convexHull,
                                               const Array<Vector3>& points,
                                               PolygonVertexArray& outPolygonVertexArray,
                                               Array<float>& outVertices, Array<unsigned int>& outIndices,
                                               Array<PolygonVertexArray::PolygonFace>& outFaces,
                                               MemoryAllocator& allocator) {

    assert(outVertices.size() == 0);
    assert(outIndices.size() == 0);
    assert(outFaces.size() == 0);

    Map<uint32, uint32> mapOldVertexIndexToNew(allocator, convexHull.getNbVertices());

    // For each face of the convex hull
    for (const QHHalfEdgeStructure::Face* face = convexHull.getFaces(); face != nullptr; face = face->nextFace) {

        PolygonVertexArray::PolygonFace polygonFace;
        polygonFace.nbVertices = 0;
        polygonFace.indexBase = outIndices.size();

        // For each edge of the face
        QHHalfEdgeStructure::Edge* firstFaceEdge = face->edge;
        QHHalfEdgeStructure::Edge* faceEdge = firstFaceEdge;
        do {

            assert(faceEdge != nullptr);

            const uint32 vOldIndex = faceEdge->startVertex->externalIndex;
            uint32 vNewIndex = outVertices.size() / 3;
            auto it = mapOldVertexIndexToNew.find(vOldIndex);

            // If the vertex is already in the new array of vertices
            if (it != mapOldVertexIndexToNew.end()) {
               vNewIndex = it->second;
            }
            else {

                // Add the vertex to the new array of vertices
                mapOldVertexIndexToNew.add(Pair<uint32, uint32>(vOldIndex, vNewIndex));
                outVertices.add(points[vOldIndex].x);
                outVertices.add(points[vOldIndex].y);
                outVertices.add(points[vOldIndex].z);
            }

            // Add the new vertex index to the array of indices
            outIndices.add(vNewIndex);

            polygonFace.nbVertices++;

            // Go to the next edge of the face
            faceEdge = faceEdge->nextFaceEdge;

        } while(faceEdge != firstFaceEdge);

        outFaces.add(polygonFace);
    }

    outPolygonVertexArray.init(outVertices.size() / 3, &(outVertices[0]), sizeof(Vector3),
                               &(outIndices[0]), sizeof(unsigned int),
                               outFaces.size(), &(outFaces[0]),
                               PolygonVertexArray::VertexDataType::VERTEX_FLOAT_TYPE,
                               PolygonVertexArray::IndexDataType::INDEX_INTEGER_TYPE);
}

// Add a vertex to the current convex hull to expand it
void QuickHull::addVertexToHull(uint32 vertexIndex,
                                QHHalfEdgeStructure::Face* face,
                                Array<Vector3>& points,
                                QHHalfEdgeStructure& convexHull,
                                decimal epsilon,
                                MemoryAllocator& allocator) {

    Array<QHHalfEdgeStructure::Vertex*> horizonVertices(allocator);
    Array<QHHalfEdgeStructure::Face*> visibleFaces(allocator);
    Array<uint32> orphanPointsIndices(allocator);

    // Find the horizon edges
    findHorizon(points[vertexIndex], face, allocator, horizonVertices, visibleFaces, epsilon);

    assert(visibleFaces.size() >= 1);
    assert(horizonVertices.size() >= 6);

    // TODO : Delete this
    std::cout << "---- Horizon Edges ----" << std::endl;
    for (uint32 i=0; i < horizonVertices.size(); i += 2) {
        const QHHalfEdgeStructure::Vertex* vertex1 = horizonVertices[i];
        const QHHalfEdgeStructure::Vertex* vertex2 = horizonVertices[i+1];
        const uint32 v1 = vertex1->externalIndex;
        const uint32 v2 = vertex2->externalIndex;
        std::cout << "Edge(" << v1 << ", " << v2 << ")" << std::endl;
    }

    assert(horizonVertices[0]->externalIndex == horizonVertices[horizonVertices.size()-1]->externalIndex);

    // Delete all the faces visible from the vertex
    deleteVisibleFaces(visibleFaces, convexHull, orphanPointsIndices, horizonVertices, allocator);

    // Build the new faces
    Array<QHHalfEdgeStructure::Face*> newFaces(allocator);
    buildNewFaces(vertexIndex, horizonVertices, convexHull, points, newFaces, allocator);

    // Merge concave faces
    mergeConcaveFaces(convexHull, newFaces, points, epsilon);

    associateOrphanPointsToNewFaces(orphanPointsIndices, newFaces, points, epsilon);
}

// Build the new faces that contain the new vertex and the horizon edges
void QuickHull::buildNewFaces(uint32 newVertexIndex,
                              Array<QHHalfEdgeStructure::Vertex*>& horizonVertices,
                              QHHalfEdgeStructure& convexHull,
                              Array<Vector3>& points,
                              Array<QHHalfEdgeStructure::Face*>& newFaces,
                              MemoryAllocator& allocator) {

    // Add the new vertex to the convex hull
    QHHalfEdgeStructure::Vertex* v = convexHull.addVertex(newVertexIndex);

    Array<QHHalfEdgeStructure::Vertex*> faceVertices(allocator, 8);

    // For each horizon edge
    for (uint32 i=0; i < horizonVertices.size(); i += 2) {

        QHHalfEdgeStructure::Vertex* v2 = horizonVertices[i];
        QHHalfEdgeStructure::Vertex* v3 = horizonVertices[i+1];

        // Create the vertices of a triangular face
        faceVertices.add(v2);
        faceVertices.add(v3);
        faceVertices.add(v);

        // Compute the face normal
        Vector3 normal = (points[v3->externalIndex] - points[v2->externalIndex]).cross(points[v->externalIndex] - points[v3->externalIndex]);
        normal.normalize();

        // Create a new face
        QHHalfEdgeStructure::Face* face = convexHull.addFace(faceVertices, normal, points, allocator);

        newFaces.add(face);

        std::cout << "Build new face with vertices: " << face->verticesString() << std::endl;

        faceVertices.clear();
    }
}

// Delete all the faces visible from the vertex to be added
void QuickHull::deleteVisibleFaces(const Array<QHHalfEdgeStructure::Face*>& visibleFaces,
                                   QHHalfEdgeStructure& convexHull,
                                   Array<uint32>& orphanPoints,
                                   const Array<QHHalfEdgeStructure::Vertex*>& horizonVertices,
                                   MemoryAllocator& allocator) {

    const uint32 nbFaces = visibleFaces.size();

    Array<QHHalfEdgeStructure::Vertex*> verticesToRemove(allocator);

    // For each visible face
    for (uint32 i=0; i < nbFaces; i++) {

        // TODO : DELETE THIS
        std::cout << "Removing visible face with vertices " << visibleFaces[i]->verticesString() << std::endl;

        // Add all the remaining points associated with this face to the array of orphan points
        orphanPoints.addRange(visibleFaces[i]->remainingClosestPoints);

        // For each vertex of the face to remove
        const QHHalfEdgeStructure::Edge* firstFaceEdge = visibleFaces[i]->edge;
        const QHHalfEdgeStructure::Edge* faceEdge = firstFaceEdge;
        do {

           QHHalfEdgeStructure::Vertex* vertex = faceEdge->startVertex;

           // If the vertex is not part of the horizon
           if (!testIsVertexInHorizon(vertex, horizonVertices)) {

               // Add the vertex to the array of vertices to be removed
               verticesToRemove.add(vertex);
           }

           faceEdge = faceEdge->nextFaceEdge;

        } while(faceEdge != firstFaceEdge);

        // Remove the face from the current convex hull
        convexHull.removeFace(visibleFaces[i]);
    }

    // Remove the vertices to be removed
    for (uint32 i=0; i < verticesToRemove.size(); i++) {
        convexHull.removeVertex(verticesToRemove[i]);
    }
}

// Return true if the vertex is part of horizon edges
bool QuickHull::testIsVertexInHorizon(QHHalfEdgeStructure::Vertex* vertex, const Array<QHHalfEdgeStructure::Vertex*>& horizonVertices) {

    // For each edge of the horizon
    for (uint32 i=0; i < horizonVertices.size(); i += 2) {

        if (horizonVertices[i] == vertex) return true;
    }

    return false;
}


// Return true if a given edge is convex and false otherwise
bool QuickHull::testIsConvexEdge(const QHHalfEdgeStructure::Edge* edge, decimal epsilon) {

    // Get the two neighbor faces
    assert(edge->twinEdge != nullptr);
    QHHalfEdgeStructure::Face* face1 = edge->face;
    QHHalfEdgeStructure::Face* face2 = edge->twinEdge->face;

    // We test if the center of face1 is below the face2 plane
    if (computePointToPlaneDistance(face1->centroid, face2->normal, face2->centroid) >= -epsilon) return false;

    // We test if the center of face2 is below the face1 plane
    if (computePointToPlaneDistance(face2->centroid, face1->normal, face1->centroid) >= -epsilon) return false;

    // If both tests are true, the edge is convex
    return true;
}

// Find the horizon (edges) forming the separation between the faces that are visible from the new vertex and the faces that are not visible
void QuickHull::findHorizon(const Vector3& vertex, QHHalfEdgeStructure::Face* face,
                            MemoryAllocator& allocator,
                            Array<QHHalfEdgeStructure::Vertex*>& outHorizonVertices,
                            Array<QHHalfEdgeStructure::Face*>& outVisibleFaces, decimal epsilon) {

    Stack<CandidateFace> facesToVisit(allocator);
    Set<const QHHalfEdgeStructure::Face*> visitedFaces(allocator);

    const QHHalfEdgeStructure::Edge* startEdge = face->edge;
    facesToVisit.push(CandidateFace(face, startEdge, startEdge));

    outVisibleFaces.add(face);

    // TODO : DELETE THIS
    std::cout << "Face visible from candidate vertex has vertices : " << face->verticesString() << std::endl;

    // While there still are faces to visit
    while (facesToVisit.size() > 0) {

        // Get the next face to process
        CandidateFace& candidateFace = facesToVisit.top();

        // Mark the current face as visited
        visitedFaces.add(candidateFace.face);

        // For each edge of the current face
        do {

            // Get the current edge to cross of the current face
            const QHHalfEdgeStructure::Edge* edge = candidateFace.currentEdge;
            assert(edge->face == candidateFace.face);

            const QHHalfEdgeStructure::Edge* twinEdge = edge->twinEdge;

            // Get the next face
            QHHalfEdgeStructure::Face* nextFace = twinEdge->face;

            // If the next face is not visited yet
            if (!visitedFaces.contains(nextFace)) {

                // If the next face is visible from the vertex
                if (nextFace->normal.dot(vertex - nextFace->centroid) > epsilon) {

                    // Add the next face to the stack of faces to visit
                    facesToVisit.push(CandidateFace(nextFace, nextFace->edge, nextFace->edge));

                    // TODO : DELETE THIS
                    std::cout << "Face visible from candidate vertex has vertices : " << nextFace->verticesString() << std::endl;

                    outVisibleFaces.add(nextFace);

                    // If the face is visible we move to visit this new visible face (Depth First Search)
                    break;
                }
                else {    // We have found a face that is not visible from the vertex

                    // We add the edge between current face and next face to the array of horizon edges
                    outHorizonVertices.add(candidateFace.currentEdge->startVertex);
                    outHorizonVertices.add(candidateFace.currentEdge->endVertex);
                }
            }

            // Move to the next edge
            candidateFace.currentEdge = edge->nextFaceEdge;

        } while (candidateFace.currentEdge != candidateFace.startEdge);

        // If we have visited all the edges of the current face
        if (candidateFace.currentEdge == candidateFace.startEdge) {

            // Remove the current face from the stack of faces to visit
            facesToVisit.pop();
        }
    }
}

// Iterate over all new faces and fix faces that are forming a concave shape in order to always keep the hull convex
void QuickHull::mergeConcaveFaces(QHHalfEdgeStructure& convexHull, Array<QHHalfEdgeStructure::Face*>& newFaces,
                                  const Array<Vector3>& points, decimal epsilon) {

    // For each new face
    for (uint32 i=0; i < newFaces.size(); i++) {

        QHHalfEdgeStructure::Face* face = newFaces[i];
        QHHalfEdgeStructure::Edge* concaveEdge;

        // While we find concave faces to be fixed
        do {

            concaveEdge = nullptr;

            // For each edge of the new face
            QHHalfEdgeStructure::Edge* firstFaceEdge = face->edge;
            QHHalfEdgeStructure::Edge* faceEdge = firstFaceEdge;
            do {

                assert(faceEdge != nullptr);

                // If the two faces at this edge are forming a convex shape
                if (testIsConvexEdge(faceEdge, epsilon)) {

                    // Move to the next edge of the face
                    faceEdge = faceEdge->nextFaceEdge;

                    continue;
                }

                // The two faces at this edge are forming a concave shape
                concaveEdge = faceEdge;
                break;

            } while(faceEdge != firstFaceEdge);

            // If we have found a concave edge
            if (concaveEdge != nullptr) {

                // Merge the two faces at this edge
                mergeConcaveFacesAtEdge(concaveEdge, convexHull, points);
            }

        } while(concaveEdge != nullptr);

    }
}

// Merge two faces that are concave at a given edge
void QuickHull::mergeConcaveFacesAtEdge(QHHalfEdgeStructure::Edge* edge, QHHalfEdgeStructure& convexHull, const Array<Vector3>& points) {

    std::cout << "Merge Concave Faces at edge with vertices (" << edge->startVertex->externalIndex << ", " << edge->endVertex->externalIndex << ")" << std::endl;

    // We merge the face next to the 'twin edge' into the face next to 'edge'

    QHHalfEdgeStructure::Face* faceToRemove = edge->twinEdge->face;
    QHHalfEdgeStructure::Face* faceToKeep = edge->face;

    // Make sure the face to keep does not reference the edge to be removed
    edge->face->edge = edge->previousFaceEdge;

    std::cout << "Removing face with vertices: " << faceToRemove->verticesString() << std::endl;
    std::cout << "Merging into face with vertices: " << faceToKeep->verticesString() << std::endl;

    // Make sure the edges of the face to delete reference the face to keep
    QHHalfEdgeStructure::Edge* firstFaceEdge = edge->twinEdge;
    QHHalfEdgeStructure::Edge* faceEdge = firstFaceEdge->nextFaceEdge;
    while (faceEdge != firstFaceEdge) {

        assert(faceEdge != nullptr);

        faceEdge->face = faceToKeep;

        // Move to the next edge of the face
        faceEdge = faceEdge->nextFaceEdge;
    };

    // Fix the linked-list of face edges
    edge->previousFaceEdge->nextFaceEdge = edge->twinEdge->nextFaceEdge;
    edge->nextFaceEdge->previousFaceEdge = edge->twinEdge->previousFaceEdge;
    edge->twinEdge->previousFaceEdge->nextFaceEdge = edge->nextFaceEdge;
    edge->twinEdge->nextFaceEdge->previousFaceEdge = edge->previousFaceEdge;

    // Move the remaining closest vertices of the face to remove to the face to keep
    faceToKeep->remainingClosestPoints.addRange(faceToRemove->remainingClosestPoints);

    // Recalculate the face centroid and normal to better fit its vertices (using Newell method)
    recalculateFace(faceToKeep, points);

    // Remove the face
    convexHull.deleteFace(faceToRemove);

    // Remove the edges
    convexHull.removeHalfEdge(edge);
    convexHull.removeHalfEdge(edge->twinEdge);

    // Fix topological issues (if any) that might have been created during merge
    fixTopologicalIssues(convexHull, faceToKeep, points);
}

// Fix topological issues (if any) that might have been created during faces merge
void QuickHull::fixTopologicalIssues(QHHalfEdgeStructure& convexHull, QHHalfEdgeStructure::Face* face, const Array<Vector3>& points) {

    // Here we want to make sure that each vertex of the convex hull has at least
    // three adjacent faces

    QHHalfEdgeStructure::Edge* edgeError;

    // While we can find an edge with an error (a redundant vertex) in the face
    do {

        edgeError = nullptr;

        // For each vertex of the face we check if the incoming and outgoing edge have
        // the same face on the opposite side
        QHHalfEdgeStructure::Edge* firstInEdge = face->edge;
        QHHalfEdgeStructure::Edge* inEdge = firstInEdge;
        while (inEdge != firstInEdge) {

            assert(inEdge != nullptr);

            QHHalfEdgeStructure::Edge* outEdge = inEdge->nextFaceEdge;

            if (inEdge->twinEdge->face == outEdge->twinEdge->face) {
                edgeError = inEdge;
                break;
            }

            // Move to the next edge of the face
            inEdge = outEdge;
        };

        // If we have found an edge with error (redundant vertex)
        if (edgeError != nullptr) {
           fixTopologicalIssueAtEdge(convexHull, face, edgeError, points);
        }

    } while (edgeError != nullptr);
}

// Fix topological issue at a given edge
void QuickHull::fixTopologicalIssueAtEdge(QHHalfEdgeStructure& convexHull, QHHalfEdgeStructure::Face* face,
                                          QHHalfEdgeStructure::Edge* inEdge, const Array<Vector3>& points) {

    std::cout << "Fix topological issue at edge" << std::endl;

    assert(inEdge->face == face);

    // If the opposite face is a triangle
    if (inEdge->twinEdge->face->isTriangle()) {

        QHHalfEdgeStructure::Face* faceToRemove = inEdge->twinEdge->face;
        QHHalfEdgeStructure::Face* faceToKeep = face;

        // Make sure the face to keep does not reference the edge to be removed
        faceToKeep->edge = inEdge->previousFaceEdge;

        // Make sure the remaining edge of the face to delete reference the face to keep
        inEdge->twinEdge->nextFaceEdge->face = faceToKeep;

        // Fix the linked-list of face edges
        inEdge->previousFaceEdge->nextFaceEdge = inEdge->twinEdge->nextFaceEdge;
        inEdge->twinEdge->nextFaceEdge->previousFaceEdge = inEdge->previousFaceEdge;
        inEdge->nextFaceEdge->nextFaceEdge->previousFaceEdge = inEdge->twinEdge->nextFaceEdge;
        inEdge->twinEdge->nextFaceEdge->nextFaceEdge = inEdge->nextFaceEdge->nextFaceEdge;

        // Move the remaining closest vertices of the face to remove to the face to keep
        faceToKeep->remainingClosestPoints.addRange(faceToRemove->remainingClosestPoints);

        // Recalculate the face centroid and normal to better fit its vertices (using Newell method)
        recalculateFace(faceToKeep, points);

        // Remove the face
        convexHull.deleteFace(faceToRemove);

        QHHalfEdgeStructure::Vertex* vertexToRemove = inEdge->endVertex;

        // Remove the edges
        convexHull.removeHalfEdge(inEdge->nextFaceEdge);
        convexHull.removeHalfEdge(inEdge->nextFaceEdge->twinEdge);
        convexHull.removeHalfEdge(inEdge);
        convexHull.removeHalfEdge(inEdge->twinEdge);

        // Remove the redundant vertex
        convexHull.removeVertex(vertexToRemove);
    }
    else {  // If the opposite face is not a triangle

        QHHalfEdgeStructure::Vertex* vertexToRemove = inEdge->endVertex;
        QHHalfEdgeStructure::Edge* outEdgeToRemove = inEdge->nextFaceEdge;

        inEdge->twinEdge->startVertex = outEdgeToRemove->endVertex;
        outEdgeToRemove->twinEdge->previousFaceEdge->nextFaceEdge = inEdge->twinEdge;
        inEdge->twinEdge->previousFaceEdge = outEdgeToRemove->twinEdge->previousFaceEdge;

        inEdge->endVertex = outEdgeToRemove->endVertex;
        inEdge->nextFaceEdge = outEdgeToRemove->nextFaceEdge;
        inEdge->nextFaceEdge->previousFaceEdge = inEdge;

        // Make sure both faces do not reference the outgoing edge we are about to remove
        face->edge = inEdge;
        inEdge->twinEdge->face->edge = inEdge->twinEdge;

        // Remove the outgoing edge
        convexHull.removeHalfEdge(outEdgeToRemove->twinEdge);
        convexHull.removeHalfEdge(outEdgeToRemove);

        // Remove the redundant vertex
        convexHull.removeVertex(vertexToRemove);

        // Recalculate the face centroid and normal to better fit its vertices (using Newell method)
        recalculateFace(face, points);
        recalculateFace(inEdge->twinEdge->face, points);
    }
}

// Recalculate the face centroid and normal to better fit its new vertices (using Newell method)
void QuickHull::recalculateFace(QHHalfEdgeStructure::Face* face, const Array<Vector3>& points) {

    Vector3 centroid(0, 0, 0);
    Vector3 normal(0, 0, 0);
    uint32 nbVertices = 0;

    std::cout << "Recalculate Face" << std::endl;
    std::cout << "Previous centroid:" << face->centroid.to_string() << std::endl;
    std::cout << "Previous normal:" << face->normal.to_string() << std::endl;

    // For each vertex of the face
    const QHHalfEdgeStructure::Edge* firstFaceEdge = face->edge;
    const QHHalfEdgeStructure::Edge* faceEdge = firstFaceEdge;
    do {

        const Vector3 v1 = points[faceEdge->startVertex->externalIndex];
        const Vector3 v2 = points[faceEdge->endVertex->externalIndex];
        centroid += v1;
        normal += Vector3((v1.y - v2.y) * (v1.z + v2.z),
                          (v1.z - v2.z) * (v1.x + v2.x),
                          (v1.x - v2.x) * (v1.y + v2.y));

        nbVertices++;

       faceEdge = faceEdge->nextFaceEdge;

    } while(faceEdge != firstFaceEdge);

    assert(nbVertices > 0);

    face->centroid = centroid / nbVertices;
    face->normal = normal.getUnit();

    std::cout << "New centroid:" << face->centroid.to_string() << std::endl;
    std::cout << "New normal:" << face->normal.to_string() << std::endl;
}

// Return the index of the next vertex candidate to be added to the hull
// This method returns INVALID_VERTEX_INDEX if there is no more vertex candidate
void QuickHull::findNextVertexCandidate(Array<Vector3>& points, uint32& outNextVertexIndex,
                                        QHHalfEdgeStructure& convexHull,
                                        QHHalfEdgeStructure::Face*& outNextFace, decimal epsilon) {

    decimal maxDistance = epsilon;
    uint32 maxVertexI = INVALID_VERTEX_INDEX;
    outNextFace = nullptr;
    outNextVertexIndex = INVALID_VERTEX_INDEX;

    // For each face
    for (auto face = convexHull.getFaces(); face != nullptr; face = face->nextFace) {

        // If the face has remaining candidates points
        if (face->remainingClosestPoints.size() > 0) {

            std::cout << "Face with vertices " << face->verticesString() << " has candidates points" << std::endl;

            const Vector3& faceNormal = face->normal;

            // For each remaining candidate point of the face
            for (uint32 i=0; i < face->remainingClosestPoints.size(); i++) {

                uint32 vertexIndex = face->remainingClosestPoints[i];

                const decimal distance = (points[vertexIndex] - face->centroid).dot(faceNormal);
                assert(distance > epsilon);

                if (distance > maxDistance) {
                    maxDistance = distance;
                    outNextVertexIndex = vertexIndex;
                    maxVertexI = i;
                    outNextFace = const_cast<QHHalfEdgeStructure::Face*>(face);
                }
            }
        }
    }

    std::cout << "Next vertex candidate: " << std::to_string(outNextVertexIndex) << " is furthest" << std::endl;

    // Remove the vertex from the array of remaining vertices for that face
    if (outNextFace != nullptr) {
        outNextFace->remainingClosestPoints.removeAt(maxVertexI);
    }
}

std::string QuickHull::showMap(Map<const QHHalfEdgeStructure::Face*, Array<uint32>>& mapFaceToRemainingClosestPoints) {

    std::string str = "Map: \n";
    for (auto it = mapFaceToRemainingClosestPoints.begin(); it != mapFaceToRemainingClosestPoints.end(); ++it) {
       str += "Face with vertices " + it->first->verticesString() + " => vertices(";

       // For each point associated to this face
       for (uint32 v = 0; v < it->second.size(); v++) {
          str += std::to_string(it->second[v]);

          if (v != it->second.size() - 1) {
            str += ',';
          }
       }

       str += ")\n";
    }

    return str;
}

// Take all the orphan points to the closest faces among the new faces
void QuickHull::associateOrphanPointsToNewFaces(Array<uint32>& orphanPointsIndices,
                                                Array<QHHalfEdgeStructure::Face*>& newFaces,
                                                Array<Vector3>& points, decimal epsilon) {

    // For each candidate points of the old faces
    for (uint32 i=0; i < orphanPointsIndices.size(); i++) {

        findClosestFaceForVertex(orphanPointsIndices[i], newFaces, points, epsilon);
    }
}

// Find the closest face for a given vertex and add this vertex to the remaining closest points for this face
void QuickHull::findClosestFaceForVertex(uint32 vertexIndex, Array<QHHalfEdgeStructure::Face*>& faces, Array<Vector3>& points, decimal epsilon) {

    decimal minDistanceToFace = DECIMAL_LARGEST;
    QHHalfEdgeStructure::Face* closestFace = nullptr;

    // For each new face
    for (uint32 f=0; f < faces.size(); f++) {

        QHHalfEdgeStructure::Face* face = faces[f];

        const decimal distanceToFace = face->normal.dot(points[vertexIndex] - face->centroid);

        // If the point is in front the face and with a smaller distance from the face
        if (distanceToFace > epsilon && distanceToFace < minDistanceToFace) {
            minDistanceToFace = distanceToFace;
            closestFace = face;
        }
    }

    // If we have found a closest face (where the point is in front of the face)
    if (closestFace != nullptr) {

        // Add the vertex to the remaining closest points of the face
        closestFace->remainingClosestPoints.add(vertexIndex);
    }
}

// Compute the initial tetrahedron convex hull
bool QuickHull::computeInitialHull(Array<Vector3>& points, QHHalfEdgeStructure& convexHull,
                                   Array<QHHalfEdgeStructure::Face*>& initialFaces,
                                   Array<uint32>& orphanPointsIndices,
                                   MemoryAllocator& allocator, std::vector<Error>& errors) {

    // Find the extreme points on each X, Y and Z axes

    uint32 extremePointsIndices[6] = {0, 0, 0, 0, 0, 0};

    const uint32 nbPoints = points.size();
    for (uint32 i=0; i < nbPoints; i++) {

        if (points[i].x < points[extremePointsIndices[0]].x) {
            extremePointsIndices[0] = i;
        }
        else if (points[i].x > points[extremePointsIndices[1]].x) {
            extremePointsIndices[1] = i;
        }

        if (points[i].y < points[extremePointsIndices[2]].y) {
            extremePointsIndices[2] = i;
        }
        else if (points[i].y > points[extremePointsIndices[3]].y) {
            extremePointsIndices[3] = i;
        }

        if (points[i].z < points[extremePointsIndices[4]].z) {
            extremePointsIndices[4] = i;
        }
        else if (points[i].z > points[extremePointsIndices[5]].z) {
            extremePointsIndices[5] = i;
        }
    }

    // Find the two extreme points with largest distance

    decimal maxLargestDistSquare = 0;
    uint32 iMax = 0;
    for (uint32 i=0; i < 3; i++) {
        const decimal distSquare = (points[extremePointsIndices[i*2]] - points[extremePointsIndices[i*2+1]]).lengthSquare();
        if (distSquare > maxLargestDistSquare) {
            iMax = i;
            maxLargestDistSquare = distSquare;
        }
    }

    if (maxLargestDistSquare < MACHINE_EPSILON) {
        errors.push_back(Error("Error during initial hull creation in QuickHull: vertices too close to each other"));
        return false;
    }

    // The pair of points that have the largest distance between them
    uint32 i1 = extremePointsIndices[iMax * 2];
    uint32 i2 = extremePointsIndices[iMax * 2 + 1];

    // Find a third point that is the furthest from line (v1, v2)
    uint32 i3 = 0;
    maxLargestDistSquare = 0;
    Vector3 lineDirection = points[i2] - points[i1];
    lineDirection.normalize();
    for (uint32 i=0; i < nbPoints; i++) {

        Vector3 vec = points[i] - points[i1];
        const decimal distSquare = vec.cross(lineDirection).lengthSquare();

        if (distSquare > maxLargestDistSquare) {
           i3 = i;
           maxLargestDistSquare = distSquare;
        }
    }

    if (maxLargestDistSquare < MACHINE_EPSILON) {
        errors.push_back(Error("Error during initial hull creation in QuickHull: vertices too close to each other"));
        return false;
    }

    // Find a fourth point that has the largest distance with the v1, v2, v3 plane

    uint32 i4 = 0;
    maxLargestDistSquare = 0;
    Vector3 planeNormal = (points[i3] - points[i1]).cross(points[i2] - points[i1]);
    for (uint32 i=0; i < nbPoints; i++) {

        Vector3 vec = points[i] - points[i1];
        const decimal distSquare = std::abs(vec.dot(planeNormal));

        if (distSquare > maxLargestDistSquare) {
           i4 = i;
           maxLargestDistSquare = distSquare;
        }
    }

    if (maxLargestDistSquare < MACHINE_EPSILON) {
        errors.push_back(Error("Error during initial hull creation in QuickHull: vertices too close to each other"));
        return false;
    }

    assert(i1 != i2 && i1 != i3 && i1 != i4);
    assert(i2 != i1 && i2 != i3 && i2 != i4);
    assert(i3 != i1 && i3 != i2 && i3 != i4);
    assert(i4 != i1 && i4 != i2 && i4 != i3);

    // Test in which side of the triangle face v1,v2,v3 is the point v4
    // to know the orientation of the tetrahedron

    QHHalfEdgeStructure::Vertex* v0 = convexHull.addVertex(i1);
    QHHalfEdgeStructure::Vertex* v1 = convexHull.addVertex(i2);
    QHHalfEdgeStructure::Vertex* v2 = convexHull.addVertex(i3);
    QHHalfEdgeStructure::Vertex* v3 = convexHull.addVertex(i4);

    Array<QHHalfEdgeStructure::Vertex*> face1Vertices(allocator);
    Array<QHHalfEdgeStructure::Vertex*> face2Vertices(allocator);
    Array<QHHalfEdgeStructure::Vertex*> face3Vertices(allocator);
    Array<QHHalfEdgeStructure::Vertex*> face4Vertices(allocator);

    const Vector3 v1V4 = points[i4] - points[i1];

    QHHalfEdgeStructure::Face* face0 = nullptr;
    QHHalfEdgeStructure::Face* face1 = nullptr;
    QHHalfEdgeStructure::Face* face2 = nullptr;
    QHHalfEdgeStructure::Face* face3 = nullptr;

    // Create the tetrahedron faces (according to the orientation of the tetrahedron)
    if (planeNormal.dot(v1V4) < 0) {

        face1Vertices.add(v0); face1Vertices.add(v2); face1Vertices.add(v1);
        face2Vertices.add(v1); face2Vertices.add(v2); face2Vertices.add(v3);
        face3Vertices.add(v0); face3Vertices.add(v1); face3Vertices.add(v3);
        face4Vertices.add(v0); face4Vertices.add(v3); face4Vertices.add(v2);

        // Compute the face normal
        const Vector3 face0Normal = planeNormal.getUnit();
        const Vector3 face1Normal = (points[i3] - points[i2]).cross(points[i4] - points[i2]).getUnit();
        const Vector3 face2Normal = (points[i2] - points[i1]).cross(points[i4] - points[i1]).getUnit();
        const Vector3 face3Normal = (points[i4] - points[i1]).cross(points[i3] - points[i1]).getUnit();

        // Add vertices for each face
        face0 = convexHull.addFace(face1Vertices, face0Normal, points, allocator);
        face1 = convexHull.addFace(face2Vertices, face1Normal, points, allocator);
        face2 = convexHull.addFace(face3Vertices, face2Normal, points, allocator);
        face3 = convexHull.addFace(face4Vertices, face3Normal, points, allocator);
    }
    else {

        face1Vertices.add(v0); face1Vertices.add(v1); face1Vertices.add(v2);
        face2Vertices.add(v1); face2Vertices.add(v3); face2Vertices.add(v2);
        face3Vertices.add(v0); face3Vertices.add(v3); face3Vertices.add(v1);
        face4Vertices.add(v0); face4Vertices.add(v2); face4Vertices.add(v3);

        // Compute the face normal
        const Vector3 face0Normal = -planeNormal.getUnit();
        const Vector3 face1Normal = (points[i4] - points[i2]).cross(points[i3] - points[i2]).getUnit();
        const Vector3 face2Normal = (points[i4] - points[i1]).cross(points[i2] - points[i1]).getUnit();
        const Vector3 face3Normal = (points[i3] - points[i1]).cross(points[i4] - points[i1]).getUnit();

        // Add vertices for each face
        face0 = convexHull.addFace(face1Vertices, face0Normal, points, allocator);
        face1 = convexHull.addFace(face2Vertices, face1Normal, points, allocator);
        face2 = convexHull.addFace(face3Vertices, face2Normal, points, allocator);
        face3 = convexHull.addFace(face4Vertices, face3Normal, points, allocator);
    }

    initialFaces.add(face0);
    initialFaces.add(face1);
    initialFaces.add(face2);
    initialFaces.add(face3);

    orphanPointsIndices.remove(v0->externalIndex);
    orphanPointsIndices.remove(v1->externalIndex);
    orphanPointsIndices.remove(v2->externalIndex);
    orphanPointsIndices.remove(v3->externalIndex);

    return true;
}

// Extract the points from the array
void QuickHull::extractPoints(const VertexArray& vertexArray, Array<Vector3>& outArray)  {

    const unsigned char* pointsStartPointer = reinterpret_cast<const unsigned char*>(vertexArray.getStart());

    if (vertexArray.getDataType() == VertexArray::DataType::VERTEX_FLOAT_TYPE) {
        for (uint32 p=0; p < vertexArray.getNbVertices(); p++) {
            const float* points = (float*)(pointsStartPointer + p * vertexArray.getStride());
            outArray.add(Vector3(points[0], points[1], points[2]));
        }
    }
    else if (vertexArray.getDataType() == VertexArray::DataType::VERTEX_DOUBLE_TYPE) {
        for (uint32 p=0; p < vertexArray.getNbVertices(); p++) {
            const double* points = (double*)(pointsStartPointer + p * vertexArray.getStride());
            outArray.add(Vector3(points[0], points[1], points[2]));
        }
    }
    else {
        assert(false);
    }
}
