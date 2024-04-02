// Copyright Contributors to the OpenVDB Project
// SPDX-License-Identifier: MPL-2.0

#include "Camera.h"

#include <cmath>

#define GLFW_INCLUDE_GLU
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

namespace openvdb_viewer {

const double Camera::sDeg2rad = M_PI / 180.0;


Camera::Camera()
    : mFov(65.0)
    , mNearPlane(0.1)
    , mFarPlane(10000.0)
    , mTarget(openvdb::Vec3d(0.0))
    , mLookAt(mTarget)
    , mUp(openvdb::Vec3d(0.0, 1.0, 0.0))
    , mForward(openvdb::Vec3d(0.0, 0.0, 1.0))
    , mRight(openvdb::Vec3d(1.0, 0.0, 0.0))
    , mEye(openvdb::Vec3d(0.0, 0.0, -1.0))
    , mTumblingSpeed(0.5)
    , mZoomSpeed(0.2)
    , mStrafeSpeed(0.05)
    , mHead(30.0)
    , mPitch(45.0)
    , mTargetDistance(25.0)
    , mDistance(mTargetDistance)
    , mMouseDown(false)
    , mStartTumbling(false)
    , mZoomMode(false)
    , mChanged(true)
    , mNeedsDisplay(true)
    , mMouseXPos(0.0)
    , mMouseYPos(0.0)
    , mWindow(nullptr)
{
}


void
Camera::setLookAtPoint(const openvdb::Vec3d& p, double dist)
{
    mLookAt = p;
    mDistance = dist;
    this->setSpeed();
    mNeedsDisplay = true;
}


void
Camera::setLookToTarget()
{
    mLookAt = mTarget;
    mDistance = mTargetDistance;
    this->setSpeed();
    mNeedsDisplay = true;
}

openvdb::Mat4s Camera::getModelView() {
    using openvdb::Vec3s, openvdb::Vec4s, openvdb::Mat4s;

    if (mChanged) {
        mChanged = false;

        mEye[0] = mLookAt[0] + mDistance * std::cos(mHead * sDeg2rad) * std::cos(mPitch * sDeg2rad);
        mEye[1] = mLookAt[1] + mDistance * std::sin(mHead * sDeg2rad);
        mEye[2] = mLookAt[2] + mDistance * std::cos(mHead * sDeg2rad) * std::sin(mPitch * sDeg2rad);

        mForward = mLookAt - mEye;
        mForward.normalize();

        mUp[1] = std::cos(mHead * sDeg2rad) > 0 ? 1.0 : -1.0;
        mRight = mForward.cross(mUp);
        mRight.normalize();
    }

    // Compute look-at MV matrix. 
    Vec3s correctedUp = mRight.cross(mForward);
    correctedUp.normalize();
    const Vec4s c0 = Vec4s(mRight[0], mRight[1], mRight[2], 0.0f);
    const Vec4s c1 = Vec4s(correctedUp[0], correctedUp[1], correctedUp[2], 0.0f);
    const Vec4s c2 = Vec4s(-mForward[0], -mForward[1], -mForward[2], 0.0f);
    Mat4s MV(c0, c1, c2, Vec4s(0.0f, 0.0f, 0.0f, 1.0f), false);
    MV.preTranslate(-mEye);

    return MV;
}

openvdb::Mat4s Camera::getProjection(const bool aFlipY) const {
    using openvdb::Vec4s, openvdb::Mat4s;

    int width, height;
    glfwGetFramebufferSize(mWindow, &width, &height);

    // Make sure that height is non-zero
    height = std::max(1, height);

    // Construct perspective projection matrix. Should be identical to gluPerspective,
    // but with the Y-axis flipped for Vulkan compatibility if `aFlipY` is true. 
    double aspectRatio = double(width) / double(height);
    
    const float ymax = float(mNearPlane * std::tan(mFov * 0.5 * sDeg2rad));
    const float xmax = float(ymax * aspectRatio);

    const float left = -xmax, right = xmax;
    const float bottom = -ymax, top = ymax;
    const float nearX2 = 2.0f*mNearPlane;

    const float A = nearX2 / (right-left);
    const float F = (aFlipY ? -1.0f : 1.0f) * nearX2 / (top-bottom);
    const float I = (right+left) / (right-left);
    const float J = (top+bottom) / (top-bottom);
    const float K = (-mFarPlane-mNearPlane) / (mFarPlane-mNearPlane);
    const float O = (-nearX2*mFarPlane) / (mFarPlane-mNearPlane);

    return Mat4s {
        A   , 0.0f, 0.0f, 0.0f ,
        0.0f, F   , 0.0f, 0.0f ,
        I   , J   , K   ,-1.0f ,
        0.0f, 0.0f, O   , 0.0f ,
    };
}


void
Camera::setSpeed(double zoomSpeed, double strafeSpeed, double tumblingSpeed)
{
    mZoomSpeed = std::max(0.0001, mDistance * zoomSpeed);
    mStrafeSpeed = std::max(0.0001, mDistance * strafeSpeed);
    mTumblingSpeed = std::clamp(tumblingSpeed, 0.2, 1.0);
}


void
Camera::setTarget(const openvdb::Vec3d& p, double dist)
{
    mTarget = p;
    mTargetDistance = dist;
}


void
Camera::aim()
{
    if (mWindow == nullptr) return;

    // Get the window size
    int width, height;
    glfwGetFramebufferSize(mWindow, &width, &height);

    // Make sure that height is non-zero to avoid division by zero
    height = std::max(1, height);

    glViewport(0, 0, width, height);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Set up the projection matrix
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();

    // Window aspect (assumes square pixels)
    double aspectRatio = double(width) / double(height);

    // Set perspective view (fov is in degrees in the y direction.)
    gluPerspective(mFov, aspectRatio, mNearPlane, mFarPlane);

    if (mChanged) {

        mChanged = false;

        mEye[0] = mLookAt[0] + mDistance * std::cos(mHead * sDeg2rad) * std::cos(mPitch * sDeg2rad);
        mEye[1] = mLookAt[1] + mDistance * std::sin(mHead * sDeg2rad);
        mEye[2] = mLookAt[2] + mDistance * std::cos(mHead * sDeg2rad) * std::sin(mPitch * sDeg2rad);

        mForward = mLookAt - mEye;
        mForward.normalize();

        mUp[1] = std::cos(mHead * sDeg2rad) > 0 ? 1.0 : -1.0;
        mRight = mForward.cross(mUp);
        mRight.normalize();
    }

    // Set up modelview matrix
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    gluLookAt(mEye[0], mEye[1], mEye[2],
              mLookAt[0], mLookAt[1], mLookAt[2],
              mUp[0], mUp[1], mUp[2]);
    mNeedsDisplay = false;
}


void
Camera::keyCallback(int key, int)
{
    if (mWindow == nullptr) return;
    int state = glfwGetKey(mWindow, key);
    switch (state) {
        case GLFW_PRESS:
            switch(key) {
                case GLFW_KEY_SPACE:
                    mZoomMode = true;
                    break;
            }
            break;
        case GLFW_RELEASE:
            switch(key) {
                case GLFW_KEY_SPACE:
                    mZoomMode = false;
                    break;
            }
            break;
    }
    mChanged = true;
}


void
Camera::mouseButtonCallback(int button, int action)
{
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_PRESS) mMouseDown = true;
        else if (action == GLFW_RELEASE) mMouseDown = false;
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        if (action == GLFW_PRESS) {
            mMouseDown = true;
            mZoomMode = true;
        } else if (action == GLFW_RELEASE) {
            mMouseDown = false;
            mZoomMode = false;
        }
    }
    if (action == GLFW_RELEASE) mMouseDown = false;

    mStartTumbling = true;
    mChanged = true;
}


void
Camera::mousePosCallback(int x, int y)
{
    if (mStartTumbling) {
        mMouseXPos = x;
        mMouseYPos = y;
        mStartTumbling = false;
    }

    double dx, dy;
    dx = x - mMouseXPos;
    dy = y - mMouseYPos;

    if (mMouseDown && !mZoomMode) {
        mNeedsDisplay = true;
        mHead += dy * mTumblingSpeed;
        mPitch += dx * mTumblingSpeed;
    } else if (mMouseDown && mZoomMode) {
        mNeedsDisplay = true;
        mLookAt += (dy * mUp - dx * mRight) * mStrafeSpeed;
    }

    mMouseXPos = x;
    mMouseYPos = y;
    mChanged = true;
}


void
Camera::mouseWheelCallback(int pos, int prevPos)
{
    double speed = std::abs(prevPos - pos);

    if (prevPos < pos) {
        mDistance += speed * mZoomSpeed;
    } else {
        double temp = mDistance - speed * mZoomSpeed;
        mDistance = std::max(0.0, temp);
    }
    setSpeed();

    mChanged = true;
    mNeedsDisplay = true;
}

} // namespace openvdb_viewer
