// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0
//
/// @file Camera.h
/// @brief Basic GL camera class

#ifndef OPENVDB_VIEWER_CAMERA_HAS_BEEN_INCLUDED
#define OPENVDB_VIEWER_CAMERA_HAS_BEEN_INCLUDED

#include <openvdb/Types.h>

struct GLFWwindow; // forward declaration


namespace openvdb_viewer {

class Camera
{
public:
    Camera();

    void setWindow(GLFWwindow* w) { mWindow = w; }

    void aim();

    void setLookAtPoint(const openvdb::Vec3d& p, double dist = 1.0);
    void setLookToTarget();

    openvdb::Mat4s getModelView();
    openvdb::Mat4s getProjection(const bool aFlipY = false) const;

    void setTarget(const openvdb::Vec3d& p, double dist = 1.0);

    void setNearFarPlanes(double n, double f) { mNearPlane = n; mFarPlane = f; }
    void setFieldOfView(double degrees) { mFov = degrees; }
    void setSpeed(double zoomSpeed = 0.1, double strafeSpeed = 0.002, double tumblingSpeed = 0.02);

    void keyCallback(int key, int action);
    void mouseButtonCallback(int button, int action);
    void mousePosCallback(int x, int y);
    void mouseWheelCallback(int pos, int prevPos);

    bool needsDisplay() const { return mNeedsDisplay; }

private:
    // Camera parameters
    double mFov, mNearPlane, mFarPlane;
    openvdb::Vec3d mTarget, mLookAt, mUp, mForward, mRight, mEye;
    double mTumblingSpeed, mZoomSpeed, mStrafeSpeed;
    double mHead, mPitch, mTargetDistance, mDistance;

    // Input states
    bool mMouseDown, mStartTumbling, mZoomMode, mChanged, mNeedsDisplay;
    double mMouseXPos, mMouseYPos;

    GLFWwindow* mWindow;

    static const double sDeg2rad;
}; // class Camera

} // namespace openvdb_viewer

#endif // OPENVDB_VIEWER_CAMERA_HAS_BEEN_INCLUDED
