#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace wowee {
namespace rendering {

struct Ray {
    glm::vec3 origin;
    glm::vec3 direction;
};

class Camera {
public:
    Camera();

    void setPosition(const glm::vec3& pos) {
        // Reject NaN/inf - would produce a NaN view matrix and freeze the
        // GPU in some drivers, or produce garbage frustum culling.
        if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return;
        position = pos; updateViewMatrix();
    }
    void setRotation(float yaw, float pitch) {
        if (!std::isfinite(yaw) || !std::isfinite(pitch)) return;
        this->yaw = yaw; this->pitch = pitch; updateViewMatrix();
    }
    void setAspectRatio(float aspect) {
        // glm::perspective with aspect <= 0 produces NaN in the projection.
        if (!std::isfinite(aspect) || aspect <= 0.0f) return;
        aspectRatio = aspect; updateProjectionMatrix();
    }
    void setFov(float fov) {
        // glm::perspective(0) is degenerate; >180 wraps the trig.
        if (!std::isfinite(fov) || fov <= 0.0f || fov >= 180.0f) return;
        this->fov = fov; updateProjectionMatrix();
    }
    /// Near and far clip, for a camera an M2 authored (a portrait camera's
    /// near plane is 0.22 yards, inside the 0.5 default).
    void setClipPlanes(float nearClip, float farClip) {
        if (!std::isfinite(nearClip) || !std::isfinite(farClip) ||
            nearClip <= 0.0f || farClip <= nearClip) return;
        nearPlane = nearClip; farPlane = farClip; updateProjectionMatrix();
    }

    [[nodiscard]] const glm::vec3& getPosition() const { return position; }
    [[nodiscard]] const glm::mat4& getViewMatrix() const { return viewMatrix; }
    [[nodiscard]] const glm::mat4& getProjectionMatrix() const { return projectionMatrix; }
    [[nodiscard]] const glm::mat4& getUnjitteredProjectionMatrix() const { return unjitteredProjectionMatrix; }
    [[nodiscard]] glm::mat4 getViewProjectionMatrix() const { return projectionMatrix * viewMatrix; }
    [[nodiscard]] glm::mat4 getUnjitteredViewProjectionMatrix() const { return unjitteredProjectionMatrix * viewMatrix; }
    [[nodiscard]] float getAspectRatio() const { return aspectRatio; }
    [[nodiscard]] float getFovDegrees() const { return fov; }
    [[nodiscard]] float getNearPlane() const { return nearPlane; }
    [[nodiscard]] float getFarPlane() const { return farPlane; }

    // Sub-pixel jitter for temporal upscaling (FSR 2)
    void setJitter(float jx, float jy);
    void clearJitter();
    [[nodiscard]] glm::vec2 getJitter() const { return jitterOffset; }

    [[nodiscard]] glm::vec3 getForward() const;
    [[nodiscard]] glm::vec3 getRight() const;
    [[nodiscard]] glm::vec3 getUp() const;

    [[nodiscard]] Ray screenToWorldRay(float screenX, float screenY, float screenW, float screenH) const;

private:
    void updateViewMatrix();
    void updateProjectionMatrix();

    glm::vec3 position = glm::vec3(0.0f);
    float yaw = 0.0f;
    float pitch = 0.0f;
    /// Vertical field of view, degrees. Stock 3.3.5a (build 12340): CGCamera
    /// is built with fov = pi/2 (0x9e8d88, ctor FUN_00607c20 from 0x606b30),
    /// and FUN_00607cb0 hands the projection (FUN_006bf370, which asserts
    /// "fovyInRadians") fov * 0.6 (0x9f5ad4) as fovy: 0.9425 rad = 54.0 deg
    /// vertical at every aspect ratio (horizontal widens with the window).
    float fov = 54.0f;
    float aspectRatio = 16.0f / 9.0f;
    float nearPlane = 0.5f;
    float farPlane = 30000.0f;   // Improves depth precision vs extremely large far clip

    glm::mat4 viewMatrix = glm::mat4(1.0f);
    glm::mat4 projectionMatrix = glm::mat4(1.0f);
    glm::mat4 unjitteredProjectionMatrix = glm::mat4(1.0f);
    glm::vec2 jitterOffset = glm::vec2(0.0f);  // NDC jitter (applied to projection)
};

} // namespace rendering
} // namespace wowee
