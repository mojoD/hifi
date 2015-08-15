//
//  RenderablePolyVoxEntityItem.cpp
//  libraries/entities-renderer/src/
//
//  Created by Seth Alves on 5/19/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <QByteArray>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif

#include <glm/gtx/quaternion.hpp>
#include <glm/gtx/string_cast.hpp>

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <DeferredLightingEffect.h>
#include <Model.h>
#include <PerfStat.h>
#include <render/Scene.h>

#include <PolyVoxCore/CubicSurfaceExtractorWithNormals.h>
#include <PolyVoxCore/MarchingCubesSurfaceExtractor.h>
#include <PolyVoxCore/SurfaceMesh.h>
#include <PolyVoxCore/SimpleVolume.h>
#include <PolyVoxCore/Raycast.h>
#include <PolyVoxCore/Material.h>

#include "model/Geometry.h"
#include "gpu/Context.h"
#include "EntityTreeRenderer.h"
#include "polyvox_vert.h"
#include "polyvox_frag.h"
#include "RenderablePolyVoxEntityItem.h"

gpu::PipelinePointer RenderablePolyVoxEntityItem::_pipeline = nullptr;

EntityItemPointer RenderablePolyVoxEntityItem::factory(const EntityItemID& entityID, const EntityItemProperties& properties) {
    return std::make_shared<RenderablePolyVoxEntityItem>(entityID, properties);
}

RenderablePolyVoxEntityItem::RenderablePolyVoxEntityItem(const EntityItemID& entityItemID,
                                                         const EntityItemProperties& properties) :
    PolyVoxEntityItem(entityItemID, properties),
    _xTexture(nullptr),
    _yTexture(nullptr),
    _zTexture(nullptr) {
    model::Mesh* mesh = new model::Mesh();
    model::MeshPointer meshPtr(mesh);
    _modelGeometry.setMesh(meshPtr);

    setVoxelVolumeSize(_voxelVolumeSize);
}

RenderablePolyVoxEntityItem::~RenderablePolyVoxEntityItem() {
    delete _volData;
}

bool inUserBounds(const PolyVox::SimpleVolume<uint8_t>* vol, PolyVoxEntityItem::PolyVoxSurfaceStyle surfaceStyle,
              int x, int y, int z) {
    // x, y, z are in user voxel-coords, not adjusted-for-edge voxel-coords.
    switch (surfaceStyle) {
        case PolyVoxEntityItem::SURFACE_MARCHING_CUBES:
        case PolyVoxEntityItem::SURFACE_CUBIC:
            if (x < 0 || y < 0 || z < 0 ||
                x >= vol->getWidth() || y >= vol->getHeight() || z >= vol->getDepth()) {
                return false;
            }
            return true;

        case PolyVoxEntityItem::SURFACE_EDGED_CUBIC:
            if (x < 0 || y < 0 || z < 0 ||
                x >= vol->getWidth() - 2 || y >= vol->getHeight() - 2 || z >= vol->getDepth() - 2) {
                return false;
            }
            return true;
    }

    return false;
}


bool inBounds(const PolyVox::SimpleVolume<uint8_t>* vol, int x, int y, int z) {
    // x, y, z are in polyvox volume coords
    return !(x < 0 || y < 0 || z < 0 || x >= vol->getWidth() || y >= vol->getHeight() || z >= vol->getDepth());
}


void RenderablePolyVoxEntityItem::setVoxelVolumeSize(glm::vec3 voxelVolumeSize) {
    if (_volData && voxelVolumeSize == _voxelVolumeSize) {
        return;
    }

    #ifdef WANT_DEBUG
    qDebug() << "resetting voxel-space size" << voxelVolumeSize.x << voxelVolumeSize.y << voxelVolumeSize.z;
    #endif

    PolyVoxEntityItem::setVoxelVolumeSize(voxelVolumeSize);

    if (_volData) {
        delete _volData;
    }

    _onCount = 0;

    if (_voxelSurfaceStyle == SURFACE_EDGED_CUBIC) {
        // with _EDGED_ we maintain an extra box of voxels around those that the user asked for.  This
        // changes how the surface extractor acts -- mainly it becomes impossible to have holes in the
        // generated mesh.  The non _EDGED_ modes will leave holes in the mesh at the edges of the
        // voxel space.
        PolyVox::Vector3DInt32 lowCorner(0, 0, 0);
        PolyVox::Vector3DInt32 highCorner(_voxelVolumeSize.x + 1, // -1 + 2 because these corners are inclusive
                                          _voxelVolumeSize.y + 1,
                                          _voxelVolumeSize.z + 1);
        _volData = new PolyVox::SimpleVolume<uint8_t>(PolyVox::Region(lowCorner, highCorner));
    } else {
        PolyVox::Vector3DInt32 lowCorner(0, 0, 0);
        PolyVox::Vector3DInt32 highCorner(_voxelVolumeSize.x - 1, // -1 because these corners are inclusive
                                          _voxelVolumeSize.y - 1,
                                          _voxelVolumeSize.z - 1);
        _volData = new PolyVox::SimpleVolume<uint8_t>(PolyVox::Region(lowCorner, highCorner));
    }

    // having the "outside of voxel-space" value be 255 has helped me notice some problems.
    _volData->setBorderValue(255);

    #ifdef WANT_DEBUG
    qDebug() << " new voxel-space size is" << _volData->getWidth() << _volData->getHeight() << _volData->getDepth();
    #endif

    // I'm not sure this is needed... the docs say that each element is initialized with its default
    // constructor.  I'll leave it here for now.
    for (int z = 0; z < _volData->getDepth(); z++) {
        for (int y = 0; y < _volData->getHeight(); y++) {
            for (int x = 0; x < _volData->getWidth(); x++) {
                _volData->setVoxelAt(x, y, z, 0);
            }
        }
    }

    // It's okay to decompress the old data here, because the data includes its original dimensions along
    // with the voxel data, and writing voxels outside the bounds of the new space is harmless.  This allows
    // adjusting of the voxel-space size without overly mangling the shape.  Shrinking the space and then
    // restoring the previous size (without any edits in between) will put the original shape back.
    decompressVolumeData();
}

void RenderablePolyVoxEntityItem::updateVoxelSurfaceStyle(PolyVoxSurfaceStyle voxelSurfaceStyle) {
    // if we are switching to or from "edged" we need to force a resize of _volData.
    if (voxelSurfaceStyle == SURFACE_EDGED_CUBIC ||
        _voxelSurfaceStyle == SURFACE_EDGED_CUBIC) {
        if (_volData) {
            delete _volData;
        }
        _volData = nullptr;
        _voxelSurfaceStyle = voxelSurfaceStyle;
        setVoxelVolumeSize(_voxelVolumeSize);
    } else {
        _voxelSurfaceStyle = voxelSurfaceStyle;
    }
    _needsModelReload = true;
}

void RenderablePolyVoxEntityItem::setVoxelData(QByteArray voxelData) {
    if (voxelData == _voxelData) {
        return;
    }
    PolyVoxEntityItem::setVoxelData(voxelData);
    decompressVolumeData();
}

glm::vec3 RenderablePolyVoxEntityItem::getSurfacePositionAdjustment() const {
    glm::vec3 scale = getDimensions() / _voxelVolumeSize; // meters / voxel-units
    switch (_voxelSurfaceStyle) {
        case PolyVoxEntityItem::SURFACE_MARCHING_CUBES:
            return scale / 2.0f;
        case PolyVoxEntityItem::SURFACE_EDGED_CUBIC:
            return scale / -2.0f;
        case PolyVoxEntityItem::SURFACE_CUBIC:
            return scale / 2.0f;
    }
    return glm::vec3(0.0f, 0.0f, 0.0f);
}

glm::mat4 RenderablePolyVoxEntityItem::voxelToLocalMatrix() const {
    glm::vec3 scale = getDimensions() / _voxelVolumeSize; // meters / voxel-units
    glm::vec3 center = getCenterPosition();
    glm::vec3 position = getPosition();
    glm::vec3 positionToCenter = center - position;
    positionToCenter -= getDimensions() * glm::vec3(0.5f, 0.5f, 0.5f) - getSurfacePositionAdjustment();
    glm::mat4 centerToCorner = glm::translate(glm::mat4(), positionToCenter);
    glm::mat4 scaled = glm::scale(centerToCorner, scale);
    return scaled;
}

glm::mat4 RenderablePolyVoxEntityItem::voxelToWorldMatrix() const {
    glm::mat4 rotation = glm::mat4_cast(getRotation());
    glm::mat4 translation = glm::translate(getPosition());
    return translation * rotation * voxelToLocalMatrix();
}

glm::mat4 RenderablePolyVoxEntityItem::worldToVoxelMatrix() const {
    glm::mat4 worldToModelMatrix = glm::inverse(voxelToWorldMatrix());
    return worldToModelMatrix;
}

uint8_t RenderablePolyVoxEntityItem::getVoxel(int x, int y, int z) {
    assert(_volData);
    if (!inUserBounds(_volData, _voxelSurfaceStyle, x, y, z)) {
        return 0;
    }

    // if _voxelSurfaceStyle is SURFACE_EDGED_CUBIC, we maintain an extra layer of
    // voxels all around the requested voxel space.  Having the empty voxels around
    // the edges changes how the surface extractor behaves.

    if (_voxelSurfaceStyle == SURFACE_EDGED_CUBIC) {
        return _volData->getVoxelAt(x + 1, y + 1, z + 1);
    }
    return _volData->getVoxelAt(x, y, z);
}

void RenderablePolyVoxEntityItem::setVoxelInternal(int x, int y, int z, uint8_t toValue) {
    // set a voxel without recompressing the voxel data
    assert(_volData);
    if (!inUserBounds(_volData, _voxelSurfaceStyle, x, y, z)) {
        return;
    }

    updateOnCount(x, y, z, toValue);

    if (_voxelSurfaceStyle == SURFACE_EDGED_CUBIC) {
        _volData->setVoxelAt(x + 1, y + 1, z + 1, toValue);
    } else {
        _volData->setVoxelAt(x, y, z, toValue);
    }
}


void RenderablePolyVoxEntityItem::setVoxel(int x, int y, int z, uint8_t toValue) {
    if (_locked) {
        return;
    }
    setVoxelInternal(x, y, z, toValue);
    compressVolumeData();
}

void RenderablePolyVoxEntityItem::updateOnCount(int x, int y, int z, uint8_t toValue) {
    // keep _onCount up to date
    if (!inUserBounds(_volData, _voxelSurfaceStyle, x, y, z)) {
        return;
    }

    uint8_t uVoxelValue = getVoxel(x, y, z);
    if (toValue != 0) {
        if (uVoxelValue == 0) {
            _onCount++;
        }
    } else {
        // toValue == 0
        if (uVoxelValue != 0) {
            _onCount--;
            assert(_onCount >= 0);
        }
    }
}

void RenderablePolyVoxEntityItem::setAll(uint8_t toValue) {
    if (_locked) {
        return;
    }

    for (int z = 0; z < _voxelVolumeSize.z; z++) {
        for (int y = 0; y < _voxelVolumeSize.y; y++) {
            for (int x = 0; x < _voxelVolumeSize.x; x++) {
                setVoxelInternal(x, y, z, toValue);
            }
        }
    }
    compressVolumeData();
}

void RenderablePolyVoxEntityItem::setVoxelInVolume(glm::vec3 position, uint8_t toValue) {
    if (_locked) {
        return;
    }

    // same as setVoxel but takes a vector rather than 3 floats.
    setVoxel((int)position.x, (int)position.y, (int)position.z, toValue);
}

void RenderablePolyVoxEntityItem::setSphereInVolume(glm::vec3 center, float radius, uint8_t toValue) {
    if (_locked) {
        return;
    }

    // This three-level for loop iterates over every voxel in the volume
    for (int z = 0; z < _voxelVolumeSize.z; z++) {
        for (int y = 0; y < _voxelVolumeSize.y; y++) {
            for (int x = 0; x < _voxelVolumeSize.x; x++) {
                // Store our current position as a vector...
                glm::vec3 pos(x + 0.5f, y + 0.5f, z + 0.5f); // consider voxels cenetered on their coordinates
                // And compute how far the current position is from the center of the volume
                float fDistToCenter = glm::distance(pos, center);
                // If the current voxel is less than 'radius' units from the center then we make it solid.
                if (fDistToCenter <= radius) {
                    updateOnCount(x, y, z, toValue);
                    setVoxelInternal(x, y, z, toValue);
                }
            }
        }
    }
    compressVolumeData();
}

void RenderablePolyVoxEntityItem::setSphere(glm::vec3 centerWorldCoords, float radiusWorldCoords, uint8_t toValue) {
    // glm::vec3 centerVoxelCoords = worldToVoxelCoordinates(centerWorldCoords);
    glm::vec4 centerVoxelCoords = worldToVoxelMatrix() * glm::vec4(centerWorldCoords, 1.0f);
    glm::vec3 scale = getDimensions() / _voxelVolumeSize; // meters / voxel-units
    float scaleY = scale.y;
    float radiusVoxelCoords = radiusWorldCoords / scaleY;
    setSphereInVolume(glm::vec3(centerVoxelCoords), radiusVoxelCoords, toValue);
}

class RaycastFunctor
{
public:
    RaycastFunctor(PolyVox::SimpleVolume<uint8_t>* vol) :
        _result(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f)),
        _vol(vol) {
    }
    bool operator()(PolyVox::SimpleVolume<unsigned char>::Sampler& sampler)
    {
        int x = sampler.getPosition().getX();
        int y = sampler.getPosition().getY();
        int z = sampler.getPosition().getZ();

        if (!inBounds(_vol, x, y, z)) {
            return true;
        }

        if (sampler.getVoxel() == 0) {
            return true; // keep raycasting
        }
        PolyVox::Vector3DInt32 positionIndex = sampler.getPosition();
        _result = glm::vec4(positionIndex.getX(), positionIndex.getY(), positionIndex.getZ(), 1.0f);
        return false;
    }
    glm::vec4 _result;
    const PolyVox::SimpleVolume<uint8_t>* _vol = nullptr;
};

bool RenderablePolyVoxEntityItem::findDetailedRayIntersection(const glm::vec3& origin,
                                                              const glm::vec3& direction,
                                                              bool& keepSearching,
                                                              OctreeElement*& element,
                                                              float& distance, BoxFace& face,
                                                              void** intersectedObject,
                                                              bool precisionPicking) const
{
    if (_needsModelReload || !precisionPicking) {
        // just intersect with bounding box
        return true;
    }

    // the PolyVox ray intersection code requires a near and far point.
    glm::mat4 wtvMatrix = worldToVoxelMatrix();
    glm::vec3 normDirection = glm::normalize(direction);

    // the PolyVox ray intersection code requires a near and far point.
    // set ray cast length to long enough to cover all of the voxel space
    float distanceToEntity = glm::distance(origin, getPosition());
    float largestDimension = glm::max(getDimensions().x, getDimensions().y, getDimensions().z) * 2.0f;
    glm::vec3 farPoint = origin + normDirection * (distanceToEntity + largestDimension);
    glm::vec4 originInVoxel = wtvMatrix * glm::vec4(origin, 1.0f);
    glm::vec4 farInVoxel = wtvMatrix * glm::vec4(farPoint, 1.0f);

    PolyVox::Vector3DFloat startPoint(originInVoxel.x, originInVoxel.y, originInVoxel.z);
    PolyVox::Vector3DFloat endPoint(farInVoxel.x, farInVoxel.y, farInVoxel.z);

    PolyVox::RaycastResult raycastResult;
    RaycastFunctor callback(_volData);
    raycastResult = PolyVox::raycastWithEndpoints(_volData, startPoint, endPoint, callback);

    if (raycastResult == PolyVox::RaycastResults::Completed) {
        // the ray completed its path -- nothing was hit.
        return false;
    }

    glm::vec4 result = callback._result;
    switch (_voxelSurfaceStyle) {
        case PolyVoxEntityItem::SURFACE_EDGED_CUBIC:
            result -= glm::vec4(1, 1, 1, 0); // compensate for the extra voxel border
            break;
        case PolyVoxEntityItem::SURFACE_MARCHING_CUBES:
        case PolyVoxEntityItem::SURFACE_CUBIC:
            break;
    }

    result -= glm::vec4(0.5f, 0.5f, 0.5f, 0.0f);

    glm::vec4 intersectedWorldPosition = voxelToWorldMatrix() * result;

    distance = glm::distance(glm::vec3(intersectedWorldPosition), origin);

    face = BoxFace::MIN_X_FACE; // XXX

    return true;
}


// compress the data in _volData and save the results.  The compressed form is used during
// saves to disk and for transmission over the wire
void RenderablePolyVoxEntityItem::compressVolumeData() {
    quint16 voxelXSize = _voxelVolumeSize.x;
    quint16 voxelYSize = _voxelVolumeSize.y;
    quint16 voxelZSize = _voxelVolumeSize.z;
    int rawSize = voxelXSize * voxelYSize * voxelZSize;

    QByteArray uncompressedData = QByteArray(rawSize, '\0');

    for (int z = 0; z < voxelZSize; z++) {
        for (int y = 0; y < voxelYSize; y++) {
            for (int x = 0; x < voxelXSize; x++) {
                uint8_t uVoxelValue = getVoxel(x, y, z);
                int uncompressedIndex =
                    z * voxelYSize * voxelXSize +
                    y * voxelXSize +
                    x;
                uncompressedData[uncompressedIndex] = uVoxelValue;
            }
        }
    }

    QByteArray newVoxelData;
    QDataStream writer(&newVoxelData, QIODevice::WriteOnly | QIODevice::Truncate);

    #ifdef WANT_DEBUG
    qDebug() << "compressing voxel data of size:" << voxelXSize << voxelYSize << voxelZSize;
    #endif

    writer << voxelXSize << voxelYSize << voxelZSize;

    QByteArray compressedData = qCompress(uncompressedData, 9);
    writer << compressedData;

    // make sure the compressed data can be sent over the wire-protocol
    if (newVoxelData.size() < 1150) {
        _voxelData = newVoxelData;
        #ifdef WANT_DEBUG
        qDebug() << "-------------- voxel compresss --------------";
        qDebug() << "raw-size =" << rawSize << "   compressed-size =" << newVoxelData.size();
        #endif
    } else {
        // HACK -- until we have a way to allow for properties larger than MTU, don't update.
        #ifdef WANT_DEBUG
        qDebug() << "voxel data too large, reverting change.";
        #endif
        // revert the active voxel-space to the last version that fit.
        decompressVolumeData();
    }

    _dirtyFlags |= EntityItem::DIRTY_SHAPE | EntityItem::DIRTY_MASS;
    _needsModelReload = true;
}


// take compressed data and expand it into _volData.
void RenderablePolyVoxEntityItem::decompressVolumeData() {
    QDataStream reader(_voxelData);
    quint16 voxelXSize, voxelYSize, voxelZSize;
    reader >> voxelXSize;
    reader >> voxelYSize;
    reader >> voxelZSize;

    if (voxelXSize == 0 || voxelXSize > MAX_VOXEL_DIMENSION ||
        voxelYSize == 0 || voxelYSize > MAX_VOXEL_DIMENSION ||
        voxelZSize == 0 || voxelZSize > MAX_VOXEL_DIMENSION) {
        qDebug() << "voxelSize is not reasonable, skipping decompressions."
                 << voxelXSize << voxelYSize << voxelZSize;
        return;
    }

    int rawSize = voxelXSize * voxelYSize * voxelZSize;

    QByteArray compressedData;
    reader >> compressedData;
    QByteArray uncompressedData = qUncompress(compressedData);

    if (uncompressedData.size() != rawSize) {
        qDebug() << "PolyVox decompress -- size is (" << voxelXSize << voxelYSize << voxelZSize << ")" <<
            "so expected uncompressed length of" << rawSize << "but length is" << uncompressedData.size();
        return;
    }

    for (int z = 0; z < voxelZSize; z++) {
        for (int y = 0; y < voxelYSize; y++) {
            for (int x = 0; x < voxelXSize; x++) {
                int uncompressedIndex = (z * voxelYSize * voxelXSize) + (y * voxelZSize) + x;
                updateOnCount(x, y, z, uncompressedData[uncompressedIndex]);
                setVoxelInternal(x, y, z, uncompressedData[uncompressedIndex]);
            }
        }
    }

    #ifdef WANT_DEBUG
    qDebug() << "--------------- voxel decompress ---------------";
    qDebug() << "raw-size =" << rawSize << "   compressed-size =" << _voxelData.size();
    #endif

    _dirtyFlags |= EntityItem::DIRTY_SHAPE | EntityItem::DIRTY_MASS;
    _needsModelReload = true;
    getModel();
}

// virtual
ShapeType RenderablePolyVoxEntityItem::getShapeType() const {
    if (_onCount > 0) {
        return SHAPE_TYPE_COMPOUND;
    }
    return SHAPE_TYPE_NONE;
}


bool RenderablePolyVoxEntityItem::isReadyToComputeShape() {
    if (_needsModelReload) {
        return false;
    }

    #ifdef WANT_DEBUG
    qDebug() << "RenderablePolyVoxEntityItem::isReadyToComputeShape" << (!_needsModelReload);
    #endif
    return true;
}

void RenderablePolyVoxEntityItem::computeShapeInfo(ShapeInfo& info) {
    #ifdef WANT_DEBUG
    qDebug() << "RenderablePolyVoxEntityItem::computeShapeInfo";
    #endif
    ShapeType type = getShapeType();
    if (type != SHAPE_TYPE_COMPOUND) {
        EntityItem::computeShapeInfo(info);
        return;
    }

    _points.clear();
    AABox box;
    glm::mat4 vtoM = voxelToLocalMatrix();

    if (_voxelSurfaceStyle == PolyVoxEntityItem::SURFACE_MARCHING_CUBES) {
        unsigned int i = 0;
        /* pull top-facing triangles into polyhedrons so they can be walked on */
        const model::MeshPointer& mesh = _modelGeometry.getMesh();
        const gpu::BufferView vertexBufferView = mesh->getVertexBuffer();
        const gpu::BufferView& indexBufferView = mesh->getIndexBuffer();
        gpu::BufferView::Iterator<const uint32_t> it = indexBufferView.cbegin<uint32_t>();
        while (it != indexBufferView.cend<uint32_t>()) {
            uint32_t p0Index = *(it++);
            uint32_t p1Index = *(it++);
            uint32_t p2Index = *(it++);

            const glm::vec3 p0 = vertexBufferView.get<const glm::vec3>(p0Index);
            const glm::vec3 p1 = vertexBufferView.get<const glm::vec3>(p1Index);
            const glm::vec3 p2 = vertexBufferView.get<const glm::vec3>(p2Index);

            qDebug() << p0Index << p1Index << p2Index;
            qDebug() << (int)p0.x << (int)p0.y << (int)p0.z;

            glm::vec3 av = (p0 + p1 + p2) / 3.0f; // center of the triangular face
            glm::vec3 normal = glm::normalize(glm::cross(p1 - p0, p2 - p0));
            float threshold = 1.0f / sqrtf(3.0f);
            if (normal.y > -threshold && normal.y < threshold) {
                // this triangle is more a wall than a floor, skip it.
            } else {
                float dropAmount = 2.0f; // XXX magic
                glm::vec3 p3 = av - glm::vec3(0.0f, dropAmount, 0.0f);

                glm::vec3 p0Model = glm::vec3(vtoM * glm::vec4(p0, 1.0f));
                glm::vec3 p1Model = glm::vec3(vtoM * glm::vec4(p1, 1.0f));
                glm::vec3 p2Model = glm::vec3(vtoM * glm::vec4(p2, 1.0f));
                glm::vec3 p3Model = glm::vec3(vtoM * glm::vec4(p3, 1.0f));

                box += p0Model;
                box += p1Model;
                box += p2Model;
                box += p3Model;

                QVector<glm::vec3> pointsInPart;
                pointsInPart << p0Model;
                pointsInPart << p1Model;
                pointsInPart << p2Model;
                pointsInPart << p3Model;
                // add next convex hull
                QVector<glm::vec3> newMeshPoints;
                _points << newMeshPoints;
                // add points to the new convex hull
                _points[i++] << pointsInPart;
            }
        }
    } else {
        unsigned int i = 0;
        for (int z = 0; z < _voxelVolumeSize.z; z++) {
            for (int y = 0; y < _voxelVolumeSize.y; y++) {
                for (int x = 0; x < _voxelVolumeSize.x; x++) {
                    if (getVoxel(x, y, z) > 0) {
                        QVector<glm::vec3> pointsInPart;

                        float offL = -0.5f;
                        float offH = 0.5f;

                        glm::vec3 p000 = glm::vec3(vtoM * glm::vec4(x + offL, y + offL, z + offL, 1.0f));
                        glm::vec3 p001 = glm::vec3(vtoM * glm::vec4(x + offL, y + offL, z + offH, 1.0f));
                        glm::vec3 p010 = glm::vec3(vtoM * glm::vec4(x + offL, y + offH, z + offL, 1.0f));
                        glm::vec3 p011 = glm::vec3(vtoM * glm::vec4(x + offL, y + offH, z + offH, 1.0f));
                        glm::vec3 p100 = glm::vec3(vtoM * glm::vec4(x + offH, y + offL, z + offL, 1.0f));
                        glm::vec3 p101 = glm::vec3(vtoM * glm::vec4(x + offH, y + offL, z + offH, 1.0f));
                        glm::vec3 p110 = glm::vec3(vtoM * glm::vec4(x + offH, y + offH, z + offL, 1.0f));
                        glm::vec3 p111 = glm::vec3(vtoM * glm::vec4(x + offH, y + offH, z + offH, 1.0f));

                        box += p000;
                        box += p001;
                        box += p010;
                        box += p011;
                        box += p100;
                        box += p101;
                        box += p110;
                        box += p111;

                        pointsInPart << p000;
                        pointsInPart << p001;
                        pointsInPart << p010;
                        pointsInPart << p011;
                        pointsInPart << p100;
                        pointsInPart << p101;
                        pointsInPart << p110;
                        pointsInPart << p111;

                        // add next convex hull
                        QVector<glm::vec3> newMeshPoints;
                        _points << newMeshPoints;
                        // add points to the new convex hull
                        _points[i++] << pointsInPart;
                    }
                }
            }
        }
    }

    if (_points.isEmpty()) {
        EntityItem::computeShapeInfo(info);
        return;
    }

    glm::vec3 collisionModelDimensions = box.getDimensions();
    QByteArray b64 = _voxelData.toBase64();
    info.setParams(type, collisionModelDimensions, QString(b64));
    info.setConvexHulls(_points);
}

void RenderablePolyVoxEntityItem::setXTextureURL(QString xTextureURL) {
    PolyVoxEntityItem::setXTextureURL(xTextureURL);
}

void RenderablePolyVoxEntityItem::setYTextureURL(QString yTextureURL) {
    PolyVoxEntityItem::setYTextureURL(yTextureURL);
}

void RenderablePolyVoxEntityItem::setZTextureURL(QString zTextureURL) {
    PolyVoxEntityItem::setZTextureURL(zTextureURL);
}

void RenderablePolyVoxEntityItem::getModel() {
    // A mesh object to hold the result of surface extraction
    PolyVox::SurfaceMesh<PolyVox::PositionMaterialNormal> polyVoxMesh;

    switch (_voxelSurfaceStyle) {
        case PolyVoxEntityItem::SURFACE_MARCHING_CUBES: {
            PolyVox::MarchingCubesSurfaceExtractor<PolyVox::SimpleVolume<uint8_t>> surfaceExtractor
                (_volData, _volData->getEnclosingRegion(), &polyVoxMesh);
            surfaceExtractor.execute();
            break;
        }
        case PolyVoxEntityItem::SURFACE_EDGED_CUBIC:
        case PolyVoxEntityItem::SURFACE_CUBIC: {
            PolyVox::CubicSurfaceExtractorWithNormals<PolyVox::SimpleVolume<uint8_t>> surfaceExtractor
                (_volData, _volData->getEnclosingRegion(), &polyVoxMesh);
            surfaceExtractor.execute();
            break;
        }
    }

    // convert PolyVox mesh to a Sam mesh
    auto mesh = _modelGeometry.getMesh();

    const std::vector<uint32_t>& vecIndices = polyVoxMesh.getIndices();
    auto indexBuffer = std::make_shared<gpu::Buffer>(vecIndices.size() * sizeof(uint32_t),
                                                     (gpu::Byte*)vecIndices.data());
    auto indexBufferPtr = gpu::BufferPointer(indexBuffer);
    auto indexBufferView = new gpu::BufferView(indexBufferPtr, gpu::Element(gpu::SCALAR, gpu::UINT32, gpu::RAW));
    mesh->setIndexBuffer(*indexBufferView);


    const std::vector<PolyVox::PositionMaterialNormal>& vecVertices = polyVoxMesh.getVertices();
    auto vertexBuffer = std::make_shared<gpu::Buffer>(vecVertices.size() * sizeof(PolyVox::PositionMaterialNormal),
                                                      (gpu::Byte*)vecVertices.data());
    auto vertexBufferPtr = gpu::BufferPointer(vertexBuffer);
    auto vertexBufferView = new gpu::BufferView(vertexBufferPtr,
                                                0,
                                                vertexBufferPtr->getSize() - sizeof(float) * 3,
                                                sizeof(PolyVox::PositionMaterialNormal),
                                                gpu::Element(gpu::VEC3, gpu::FLOAT, gpu::RAW));
    mesh->setVertexBuffer(*vertexBufferView);
    mesh->addAttribute(gpu::Stream::NORMAL,
                       gpu::BufferView(vertexBufferPtr,
                                       sizeof(float) * 3,
                                       vertexBufferPtr->getSize() - sizeof(float) * 3,
                                       sizeof(PolyVox::PositionMaterialNormal),
                                       gpu::Element(gpu::VEC3, gpu::FLOAT, gpu::RAW)));

    #ifdef WANT_DEBUG
    qDebug() << "---- vecIndices.size() =" << vecIndices.size();
    qDebug() << "---- vecVertices.size() =" << vecVertices.size();
    #endif

    _needsModelReload = false;
}

void RenderablePolyVoxEntityItem::render(RenderArgs* args) {
    PerformanceTimer perfTimer("RenderablePolyVoxEntityItem::render");
    assert(getType() == EntityTypes::PolyVox);
    Q_ASSERT(args->_batch);

    if (!_pipeline) {
        gpu::ShaderPointer vertexShader = gpu::ShaderPointer(gpu::Shader::createVertex(std::string(polyvox_vert)));
        gpu::ShaderPointer pixelShader = gpu::ShaderPointer(gpu::Shader::createPixel(std::string(polyvox_frag)));

        gpu::Shader::BindingSet slotBindings;
        slotBindings.insert(gpu::Shader::Binding(std::string("materialBuffer"), MATERIAL_GPU_SLOT));
        slotBindings.insert(gpu::Shader::Binding(std::string("xMap"), 0));
        slotBindings.insert(gpu::Shader::Binding(std::string("yMap"), 1));
        slotBindings.insert(gpu::Shader::Binding(std::string("zMap"), 2));

        gpu::ShaderPointer program = gpu::ShaderPointer(gpu::Shader::createProgram(vertexShader, pixelShader));
        gpu::Shader::makeProgram(*program, slotBindings);

        auto state = std::make_shared<gpu::State>();
        state->setCullMode(gpu::State::CULL_BACK);
        state->setDepthTest(true, true, gpu::LESS_EQUAL);

        _pipeline = gpu::PipelinePointer(gpu::Pipeline::create(program, state));
    }

    if (_needsModelReload) {
        getModel();
    }

    gpu::Batch& batch = *args->_batch;
    batch.setPipeline(_pipeline);

    auto mesh = _modelGeometry.getMesh();
    Transform transform(voxelToWorldMatrix());
    batch.setModelTransform(transform);
    batch.setInputFormat(mesh->getVertexFormat());
    batch.setInputBuffer(gpu::Stream::POSITION, mesh->getVertexBuffer());
    batch.setInputBuffer(gpu::Stream::NORMAL,
                         mesh->getVertexBuffer()._buffer,
                         sizeof(float) * 3,
                         mesh->getVertexBuffer()._stride);
    batch.setIndexBuffer(gpu::UINT32, mesh->getIndexBuffer()._buffer, 0);

    if (!_xTextureURL.isEmpty() && !_xTexture) {
        _xTexture = DependencyManager::get<TextureCache>()->getTexture(_xTextureURL);
    }
    if (!_yTextureURL.isEmpty() && !_yTexture) {
        _yTexture = DependencyManager::get<TextureCache>()->getTexture(_yTextureURL);
    }
    if (!_zTextureURL.isEmpty() && !_zTexture) {
        _zTexture = DependencyManager::get<TextureCache>()->getTexture(_zTextureURL);
    }

    batch._glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

    if (_xTexture) {
        batch.setResourceTexture(0, _xTexture->getGPUTexture());
    } else {
        batch.setResourceTexture(0, DependencyManager::get<TextureCache>()->getWhiteTexture());
    }
    if (_yTexture) {
        batch.setResourceTexture(1, _yTexture->getGPUTexture());
    } else {
        batch.setResourceTexture(1, DependencyManager::get<TextureCache>()->getWhiteTexture());
    }
    if (_zTexture) {
        batch.setResourceTexture(2, _zTexture->getGPUTexture());
    } else {
        batch.setResourceTexture(2, DependencyManager::get<TextureCache>()->getWhiteTexture());
    }

    int voxelVolumeSizeLocation = _pipeline->getProgram()->getUniforms().findLocation("voxelVolumeSize");
    batch._glUniform3f(voxelVolumeSizeLocation, _voxelVolumeSize.x, _voxelVolumeSize.y, _voxelVolumeSize.z);

    batch.drawIndexed(gpu::TRIANGLES, mesh->getNumIndices(), 0);

    RenderableDebugableEntityItem::render(this, args);
}

bool RenderablePolyVoxEntityItem::addToScene(EntityItemPointer self,
                                             std::shared_ptr<render::Scene> scene,
                                             render::PendingChanges& pendingChanges) {
    _myItem = scene->allocateID();

    auto renderItem = std::make_shared<PolyVoxPayload>(shared_from_this());
    auto renderData = PolyVoxPayload::Pointer(renderItem);
    auto renderPayload = std::make_shared<PolyVoxPayload::Payload>(renderData);

    pendingChanges.resetItem(_myItem, renderPayload);

    return true;
}

void RenderablePolyVoxEntityItem::removeFromScene(EntityItemPointer self,
                                                  std::shared_ptr<render::Scene> scene,
                                                  render::PendingChanges& pendingChanges) {
    pendingChanges.removeItem(_myItem);
}

namespace render {
    template <> const ItemKey payloadGetKey(const PolyVoxPayload::Pointer& payload) {
        return ItemKey::Builder::opaqueShape();
    }

    template <> const Item::Bound payloadGetBound(const PolyVoxPayload::Pointer& payload) {
        if (payload && payload->_owner) {
            auto polyVoxEntity = std::dynamic_pointer_cast<RenderablePolyVoxEntityItem>(payload->_owner);
            return polyVoxEntity->getAABox();
        }
        return render::Item::Bound();
    }

    template <> void payloadRender(const PolyVoxPayload::Pointer& payload, RenderArgs* args) {
        if (args && payload && payload->_owner) {
            payload->_owner->render(args);
        }
    }
}
