
#include "camera.h"
#include "math/transform.h"
#include "defs.h"
#include "../graphics/graphics.h"
#include "../math/mathf.h"

int isOutsideFrustrum(struct FrustrumCullingInformation* frustrum, struct BoundingBoxs16* boundingBox) {
    for (int i = 0; i < frustrum->usedClippingPlaneCount; ++i) {
        struct Vector3 closestPoint;

        struct Vector3* normal = &frustrum->clippingPlanes[i].normal;

        closestPoint.x = normal->x < 0.0f ? boundingBox->minX : boundingBox->maxX;
        closestPoint.y = normal->y < 0.0f ? boundingBox->minY : boundingBox->maxY;
        closestPoint.z = normal->z < 0.0f ? boundingBox->minZ : boundingBox->maxZ;

        if (planePointDistance(&frustrum->clippingPlanes[i], &closestPoint) < 0.00001f) {
            return 1;
        }
    }


    return 0;
}

int isSphereOutsideFrustrum(struct FrustrumCullingInformation* frustrum, struct Vector3* scaledCenter, float scaledRadius) {
    for (int i = 0; i < frustrum->usedClippingPlaneCount; ++i) {
        if (planePointDistance(&frustrum->clippingPlanes[i], scaledCenter) < -scaledRadius) {
            return 1;
        }
    }

    return 0;
}

void cameraInit(struct Camera* camera, float fov, float near, float far) {
    transformInitIdentity(&camera->transform);
    camera->fov = fov;
    camera->nearPlane = near;
    camera->farPlane = far;
}

void cameraBuildViewMatrix(struct Camera* camera, float matrix[4][4]) {
    struct Transform cameraTransCopy = camera->transform;
    vector3Scale(&cameraTransCopy.position, &cameraTransCopy.position, SCENE_SCALE);
    struct Transform inverse;
    transformInvert(&cameraTransCopy, &inverse);
    transformToMatrix(&inverse, matrix, 1.0f);
}

void cameraBuildProjectionMatrix(struct Camera* camera, float matrix[4][4], u16* perspectiveNormalize, float aspectRatio) {
    float planeScalar = 1.0f;

    if (camera->transform.position.y > camera->farPlane * 0.5f) {
        planeScalar = 2.0f * camera->transform.position.y / camera->farPlane;
    }

    guPerspectiveF(matrix, perspectiveNormalize, camera->fov, aspectRatio, camera->nearPlane * planeScalar, camera->farPlane * planeScalar, 1.0f);
}

void cameraExtractClippingPlane(float viewPersp[4][4], struct Plane* output, int axis, float direction) {
    output->normal.x = viewPersp[0][axis] * direction + viewPersp[0][3];
    output->normal.y = viewPersp[1][axis] * direction + viewPersp[1][3];
    output->normal.z = viewPersp[2][axis] * direction + viewPersp[2][3];
    output->d = viewPersp[3][axis] * direction + viewPersp[3][3];

    float mult = 1.0f / sqrtf(vector3MagSqrd(&output->normal));
    vector3Scale(&output->normal, &output->normal, mult);
    output->d *= mult;
}

int cameraIsValidMatrix(float matrix[4][4]) {
    return fabsf(matrix[3][0]) <= 0x7fff && fabsf(matrix[3][1]) <= 0x7fff && fabsf(matrix[3][2]) <= 0x7fff;
}

int cameraSetupMatrices(struct Camera* camera, struct RenderState* renderState, float aspectRatio, Vp* viewport, int extractClippingPlanes, struct CameraMatrixInfo* output) {
    float view[4][4];
    float persp[4][4];
    float combined[4][4];

    float scaleX = viewport->vp.vscale[0] * (1.0f / (SCREEN_WD << 1));
    float scaleY = viewport->vp.vscale[1] * (1.0f / (SCREEN_HT << 1));

    float centerX = ((float)viewport->vp.vtrans[0] - (SCREEN_WD << 1)) * (1.0f / (SCREEN_WD << 1));
    float centerY = ((SCREEN_HT << 1) - (float)viewport->vp.vtrans[1]) * (1.0f / (SCREEN_HT << 1));

    guOrthoF(combined, centerX - scaleX, centerX + scaleX, centerY - scaleY, centerY + scaleY, 1.0f, -1.0f, 1.0f);
    cameraBuildProjectionMatrix(camera, view, &output->perspectiveNormalize, aspectRatio);
    guMtxCatF(view, combined, persp);

    cameraBuildViewMatrix(camera, view);
    guMtxCatF(view, persp, combined);

    if (!cameraIsValidMatrix(combined)) {
        goto error;
    }

    output->projectionView = renderStateRequestMatrices(renderState, 1);

    if (!output->projectionView) {
        return 0;
    }

    guMtxF2L(combined, output->projectionView);

    if (extractClippingPlanes) {
        cameraExtractClippingPlane(combined, &output->cullingInformation.clippingPlanes[0], 0, 1.0f);
        cameraExtractClippingPlane(combined, &output->cullingInformation.clippingPlanes[1], 0, -1.0f);
        cameraExtractClippingPlane(combined, &output->cullingInformation.clippingPlanes[2], 1, 1.0f);
        cameraExtractClippingPlane(combined, &output->cullingInformation.clippingPlanes[3], 1, -1.0f);
        cameraExtractClippingPlane(combined, &output->cullingInformation.clippingPlanes[4], 2, 1.0f);
        output->cullingInformation.cameraPos = camera->transform.position;
        output->cullingInformation.usedClippingPlaneCount = 5;
    }

    return 1;
error:
    return 0;
}

int cameraApplyMatrices(struct RenderState* renderState, struct CameraMatrixInfo* matrixInfo) {
    Mtx* modelMatrix = renderStateRequestMatrices(renderState, 1);
    
    if (!modelMatrix) {
        return 0;
    }

    guMtxIdent(modelMatrix);
    gSPMatrix(renderState->dl++, osVirtualToPhysical(modelMatrix), G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);

    gSPMatrix(renderState->dl++, osVirtualToPhysical(matrixInfo->projectionView), G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    gSPPerspNormalize(renderState->dl++, matrixInfo->perspectiveNormalize);

    return 1;
}

// assuming projection matrix works as follows
// a 0 0                    0
// 0 b 0                    0
// 0 0 (n + f) / (n - f)    2 * n * f / (n - f)
// 0 0 -1                   0

// distance should be a positive value not scaled by scene scale
// returns -1 for the near plane
// returns 1 for the far plane
float cameraClipDistance(struct Camera* camera, float distance) {
    float modifiedDistance = distance * -SCENE_SCALE;
    
    float denom = modifiedDistance * (camera->nearPlane - camera->farPlane);

    if (fabsf(denom) < 0.00000001f) {
        return 0.0f;
    }

    return -((camera->nearPlane + camera->farPlane) * modifiedDistance + 2.0f * camera->nearPlane * camera->farPlane) / denom;
}