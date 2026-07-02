/**
 * @file fsposingmotion.cpp
 * @brief Model for posing your (and other) avatar(s).
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (c) 2024 Angeldark Raymaker @ Second Life
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include <deque>
#include <boost/algorithm/string.hpp>
#include "fsposingmotion.h"
#include "llcharacter.h"
#include "llviewercontrol.h"
#include "llmotioncontroller.h"

LLJoint::JointPriority FSPosingMotion::getPriority()
{
    if (gSavedSettings.getBOOL("FSPoserLiveOverlay"))
        return LLJoint::LOW_PRIORITY; // let animation win

    S32 pri = gSavedSettings.getS32("FSPoserPriority");
    if (pri < 0) pri = 0;
    if (pri > 7) pri = 7;
    return (LLJoint::JointPriority)pri;
}

FSPosingMotion::FSPosingMotion(const LLUUID& id) : LLKeyframeMotion(id)
{
    mName = "fs_poser_pose";
    mMotionID = id;
    mJointMotionList = &dummyMotionList;
}

LLMotion::LLMotionInitStatus FSPosingMotion::onInitialize(LLCharacter* character)
{
    if (!character)
        return STATUS_FAILURE;

    mJointPoses.clear();

    LLJoint* targetJoint;
    for (S32 i = 0; (targetJoint = character->getCharacterJoint(i)); ++i)
    {
        if (!targetJoint)
            continue;

        FSJointPose jointPose = FSJointPose(targetJoint, POSER_JOINT_STATE);
        mJointPoses.push_back(jointPose);

        addJointState(jointPose.getJointState());
    }

    for (S32 i = 0; (targetJoint = character->findCollisionVolume(i)); ++i)
    {
        if (!targetJoint)
            continue;

        FSJointPose jointPose = FSJointPose(targetJoint, POSER_JOINT_STATE, true);
        mJointPoses.push_back(jointPose);

        addJointState(jointPose.getJointState());
    }

    return STATUS_SUCCESS;
}

bool FSPosingMotion::onActivate()
{
    return true;
}

bool FSPosingMotion::onUpdate(F32 time, U8* joint_mask)
{
    bool live = gSavedSettings.getBOOL("FSPoserLiveOverlay");

    if (live)
    {
        // Live overlay: for joints the user has modified, set identity
        // so the Poser doesn't contaminate the blend with last frame's
        // onPostBlend output. Unmodified joints mirror normally.
        for (FSJointPose jointPose : mJointPoses)
        {
            LLJoint* joint = jointPose.getJointState()->getJoint();
            if (!joint) continue;
            jointPose.getJointState()->setPriority(LLJoint::LOW_PRIORITY);
            if (jointPose.getPublicRotation() != LLQuaternion::DEFAULT ||
                !jointPose.getPublicPosition().isExactlyZero() ||
                !jointPose.getPublicScale().isExactlyZero())
            {
                jointPose.getJointState()->setRotation(LLQuaternion::DEFAULT);
                jointPose.getJointState()->setPosition(joint->getPosition());
                jointPose.getJointState()->setScale(joint->getScale());
            }
            else
            {
                jointPose.getJointState()->setRotation(joint->getRotation());
                jointPose.getJointState()->setPosition(joint->getPosition());
                jointPose.getJointState()->setScale(joint->getScale());
            }
        }
        return true;
    }

    // Frozen mode: restore configured priority, interpolate toward target
    S32 cfg_pri = gSavedSettings.getS32("FSPoserPriority");
    if (cfg_pri < 0) cfg_pri = 0;
    if (cfg_pri > 7) cfg_pri = 7;
    for (FSJointPose jp : mJointPoses)
        jp.getJointState()->setPriority((LLJoint::JointPriority)cfg_pri);

    LLQuaternion targetRotation;
    LLQuaternion currentRotation;
    LLVector3 currentPosition;
    LLVector3 targetPosition;
    LLVector3 currentScale;
    LLVector3 targetScale;

    for (FSJointPose jointPose : mJointPoses)
    {
        LLJoint* joint = jointPose.getJointState()->getJoint();
        if (!joint)
            continue;

        currentRotation = joint->getRotation();
        currentPosition = joint->getPosition();
        currentScale = joint->getScale();
        targetRotation  = jointPose.getTargetRotation();
        targetPosition  = jointPose.getTargetPosition();
        targetScale     = jointPose.getTargetScale();

        if (vectorsNotQuiteEqual(currentPosition, targetPosition))
        {
            currentPosition = lerp(currentPosition, targetPosition, mInterpolationTime);
            jointPose.getJointState()->setPosition(currentPosition);
        }

        if (quatsNotQuiteEqual(currentRotation, targetRotation))
        {
            currentRotation = slerp(mInterpolationTime, currentRotation, targetRotation);
            jointPose.getJointState()->setRotation(currentRotation);
        }

        if (vectorsNotQuiteEqual(currentScale, targetScale))
        {
            currentScale = lerp(currentScale, targetScale, mInterpolationTime);
            jointPose.getJointState()->setScale(currentScale);
        }
    }

    return true;
}

void FSPosingMotion::onDeactivate() { revertJointsAndCollisionVolumes(); }

void FSPosingMotion::onPostBlend()
{
    if (!gSavedSettings.getBOOL("FSPoserLiveOverlay"))
        return;

    for (FSJointPose& jointPose : mJointPoses)
    {
        LLJoint* joint = jointPose.getJointState()->getJoint();
        if (!joint) continue;

        LLQuaternion animRot = joint->getRotation();
        LLVector3    animPos = joint->getPosition();
        LLQuaternion pubRot  = jointPose.getPublicRotation();
        if (pubRot != LLQuaternion::DEFAULT)
        {
            joint->setRotation(animRot * pubRot);
            // Rotate animation position into user's frame too
            joint->setPosition(animPos * pubRot);
        }

        LLVector3 pubPos  = jointPose.getPublicPosition();
        if (!pubPos.isExactlyZero())
            joint->setPosition(joint->getPosition() + pubPos);

        LLVector3 animScl = joint->getScale();
        LLVector3 pubScl  = jointPose.getPublicScale();
        if (!pubScl.isExactlyZero())
            joint->setScale(animScl.scaledVec(pubScl));
    }
}

void FSPosingMotion::revertJointsAndCollisionVolumes()
{
    for (FSJointPose jointPose : mJointPoses)
    {
        jointPose.revertJoint();

        LLJoint* joint = jointPose.getJointState()->getJoint();
        if (!joint)
            continue;

        addJointToState(joint);
    }
}

bool FSPosingMotion::currentlyPosingJoint(FSJointPose* joint)
{
    if (!joint)
        return false;

    LLJoint* avJoint = joint->getJointState()->getJoint();
    if (!avJoint)
        return false;

    return currentlyPosingJoint(avJoint);
}

void FSPosingMotion::addJointToState(FSJointPose* joint)
{
    if (!joint)
        return;

    LLJoint* avJoint = joint->getJointState()->getJoint();
    if (!avJoint)
        return;

    setJointState(avJoint, POSER_JOINT_STATE);
}

void FSPosingMotion::removeJointFromState(FSJointPose* joint)
{
    if (!joint)
        return;

    LLJoint* avJoint = joint->getJointState()->getJoint();
    if (!avJoint)
        return;

    joint->revertJoint();

    setJointState(avJoint, 0);
}

void FSPosingMotion::addJointToState(LLJoint* joint)
{
    setJointState(joint, POSER_JOINT_STATE);
}

void FSPosingMotion::removeJointFromState(LLJoint* joint)
{
    setJointState(joint, 0);
}

void FSPosingMotion::setJointState(LLJoint* joint, U32 state)
{
    if (mJointPoses.empty())
        return;
    if (!joint)
        return;

    LLPose* pose = this->getPose();
    if (!pose)
        return;

    LLPointer<LLJointState> jointState = pose->findJointState(joint);
    if (jointState.isNull())
        return;

    pose->removeJointState(jointState);
    FSJointPose *jointPose = getJointPoseByJointName(joint->getName());
    if (!jointPose)
        return;

    jointPose->getJointState()->setUsage(state);
    addJointState(jointPose->getJointState());
}

FSJointPose* FSPosingMotion::getJointPoseByJointName(const std::string& name)
{
    if (name.empty() || mJointPoses.empty())
        return nullptr;

    for (auto poserJoint_iter = mJointPoses.begin(); poserJoint_iter != mJointPoses.end(); ++poserJoint_iter)
    {
        if (!boost::iequals(poserJoint_iter->jointName(), name))
            continue;

        return &*poserJoint_iter;
    }

    return nullptr;
}

FSJointPose* FSPosingMotion::getJointPoseByJointNumber(const S32 number)
{
    if (mJointPoses.empty())
        return nullptr;
    if (number < 0)
        return nullptr;

    for (auto poserJoint_iter = mJointPoses.begin(); poserJoint_iter != mJointPoses.end(); ++poserJoint_iter)
    {
        if (poserJoint_iter->getJointNumber() != number)
            continue;

        return &*poserJoint_iter;
    }

    return nullptr;
}

bool FSPosingMotion::currentlyPosingJoint(LLJoint* joint)
{
    if (mJointPoses.empty())
        return false;

    if (!joint)
        return false;

    LLPose* pose = getPose();
    if (!pose)
        return false;

    LLPointer<LLJointState> jointState = pose->findJointState(joint);
    if (jointState.isNull())
        return false;

    U32 state = jointState->getUsage();
    return (state & POSER_JOINT_STATE);
}

bool FSPosingMotion::allStartingRotationsAreZero() const
{
    for (auto poserJoint_iter = mJointPoses.begin(); poserJoint_iter != mJointPoses.end(); ++poserJoint_iter)
    {
        if (poserJoint_iter->isCollisionVolume())
            continue;

        if (!poserJoint_iter->isBaseRotationZero())
            return false;
    }

    return true;
}

void FSPosingMotion::setAllRotationsToZeroAndClearUndo()
{
    for (auto poserJoint_iter = mJointPoses.begin(); poserJoint_iter != mJointPoses.end(); ++poserJoint_iter)
    {
        poserJoint_iter->purgeUndoQueue();
        poserJoint_iter->setPublicRotation(true, true, POSER_CHANGE_ROTATION, LLQuaternion::DEFAULT);
    }
}

void FSPosingMotion::setJointBvhLock(FSJointPose* joint, bool lockInBvh)
{
    joint->zeroBaseRotation(lockInBvh);
}

bool FSPosingMotion::loadOtherMotionToBaseOfThisMotion(LLKeyframeMotion* motionToLoad, F32 timeToLoadAt, const std::vector<S32>& selectedJointNumbers)
{
    FSPosingMotion* motionToLoadAsFsPosingMotion = static_cast<FSPosingMotion*>(motionToLoad);
    if (!motionToLoadAsFsPosingMotion)
        return false;

    LLJoint::JointPriority priority = motionToLoad->getPriority();
    bool                   motionIsForAllJoints = selectedJointNumbers.empty();

    LLQuaternion rot;
    LLVector3    position, scale;
    bool         hasRotation = false, hasPosition = false, hasScale = false;

    for (auto poserJoint_iter = mJointPoses.begin(); poserJoint_iter != mJointPoses.end(); ++poserJoint_iter)
    {
        S32         jointNumber = poserJoint_iter->getJointNumber();
        std::string jointName   = poserJoint_iter->jointName();

        bool motionIsForThisJoint =
            std::find(selectedJointNumbers.begin(), selectedJointNumbers.end(), jointNumber) != selectedJointNumbers.end();
        if (!motionIsForAllJoints && !motionIsForThisJoint)
            continue;

        hasRotation = hasPosition = hasScale = false;
        motionToLoadAsFsPosingMotion->getJointStateAtTime(jointName, timeToLoadAt, &hasRotation, &rot, &hasPosition, &position, &hasScale, &scale);

        if (hasRotation && !poserJoint_iter->userHasSetBaseRotationToZero())
            poserJoint_iter->setBaseRotation(rot, priority);

        if (hasPosition)
            poserJoint_iter->setBasePosition(position, priority);

        if (hasScale)
            poserJoint_iter->setBaseScale(scale, priority);
    }

    return true;
}

void FSPosingMotion::getJointStateAtTime(std::string jointPoseName, F32 timeToLoadAt,
                                            bool* hasRotation, LLQuaternion* jointRotation,
                                            bool* hasPosition, LLVector3* jointPosition,
                                            bool* hasScale,    LLVector3* jointScale)
{
    if ( mJointMotionList == nullptr)
        return;

    for (U32 i = 0; i < mJointMotionList->getNumJointMotions(); i++)
    {
        JointMotion* jm = mJointMotionList->getJointMotion(i);
        if (!boost::iequals(jointPoseName, jm->mJointName))
            continue;

        *hasRotation = (jm->mRotationCurve.mNumKeys > 0);
        if (hasRotation)
            jointRotation->set(jm->mRotationCurve.getValue(timeToLoadAt, mJointMotionList->mDuration));

        *hasPosition = (jm->mPositionCurve.mNumKeys > 0);
        if (hasPosition)
            jointPosition->set(jm->mPositionCurve.getValue(timeToLoadAt, mJointMotionList->mDuration));

        *hasScale = (jm->mScaleCurve.mNumKeys > 0);
        if (hasScale)
            jointScale->set(jm->mScaleCurve.getValue(timeToLoadAt, mJointMotionList->mDuration));

        return;
    }
}

bool FSPosingMotion::otherMotionAnimatesJoints(LLKeyframeMotion* motionToQuery, const std::vector<S32>& recapturedJointNumbers)
{
    FSPosingMotion* motionToLoadAsFsPosingMotion = static_cast<FSPosingMotion*>(motionToQuery);
    if (!motionToLoadAsFsPosingMotion)
        return false;

    return motionToLoadAsFsPosingMotion->motionAnimatesJoints(recapturedJointNumbers);
}

// Do not try to access FSPosingMotion state; you are a LLKeyframeMotion cast as a FSPosingMotion, NOT an FSPosingMotion.
bool FSPosingMotion::motionAnimatesJoints(const std::vector<S32>& recapturedJointNumbers)
{
    if (mJointMotionList == nullptr)
        return false;

    for (U32 i = 0; i < mJointMotionList->getNumJointMotions(); i++)
    {
        JointMotion* jm = mJointMotionList->getJointMotion(i);
        LLJoint*     joint = mCharacter->getJoint(jm->mJointName);

        if (std::find(recapturedJointNumbers.begin(), recapturedJointNumbers.end(), joint->getJointNum()) == recapturedJointNumbers.end())
            continue;

        if (jm->mRotationCurve.mNumKeys > 0)
            return true;
    }

    return false;
}

void FSPosingMotion::resetBonePriority(const std::vector<S32>& boneNumbersToReset)
{
    for (S32 boneNumber : boneNumbersToReset)
    {
        for (auto poserJoint_iter = mJointPoses.begin(); poserJoint_iter != mJointPoses.end(); ++poserJoint_iter)
        {
            if (poserJoint_iter->getJointNumber() == boneNumber)
                poserJoint_iter->setJointPriority(LLJoint::LOW_PRIORITY);
        }
    }
}

bool FSPosingMotion::vectorsNotQuiteEqual(const LLVector3& v1, const LLVector3& v2) const
{
    if (vectorAxesAlmostEqual(v1.mV[VX], v2.mV[VX]) &&
        vectorAxesAlmostEqual(v1.mV[VY], v2.mV[VY]) &&
        vectorAxesAlmostEqual(v1.mV[VZ], v2.mV[VZ]))
        return false;

    return true;
}

bool FSPosingMotion::quatsNotQuiteEqual(const LLQuaternion& q1, const LLQuaternion& q2) const
{
    if (vectorAxesAlmostEqual(q1.mQ[VW], q2.mQ[VW]) &&
        vectorAxesAlmostEqual(q1.mQ[VX], q2.mQ[VX]) &&
        vectorAxesAlmostEqual(q1.mQ[VY], q2.mQ[VY]) &&
        vectorAxesAlmostEqual(q1.mQ[VZ], q2.mQ[VZ]))
        return false;

    if (vectorAxesAlmostEqual(q1.mQ[VW], -q2.mQ[VW]) &&
        vectorAxesAlmostEqual(q1.mQ[VX], -q2.mQ[VX]) &&
        vectorAxesAlmostEqual(q1.mQ[VY], -q2.mQ[VY]) &&
        vectorAxesAlmostEqual(q1.mQ[VZ], -q2.mQ[VZ]))
        return false;

    return true;
}

LLSD FSPosingMotion::toLLSD() const
{
    LLSD result = LLSD::emptyArray();
    for (auto joint_iter = mJointPoses.begin(); joint_iter != mJointPoses.end(); ++joint_iter)
    {
        const FSJointPose& joint = *joint_iter;
        LLSD entry;
        entry["n"] = joint.jointName();
        entry["rx"] = joint.getPublicRotation().mQ[VX];
        entry["ry"] = joint.getPublicRotation().mQ[VY];
        entry["rz"] = joint.getPublicRotation().mQ[VZ];
        entry["rw"] = joint.getPublicRotation().mQ[VW];
        entry["px"] = joint.getPublicPosition().mV[VX];
        entry["py"] = joint.getPublicPosition().mV[VY];
        entry["pz"] = joint.getPublicPosition().mV[VZ];
        entry["sx"] = joint.getPublicScale().mV[VX];
        entry["sy"] = joint.getPublicScale().mV[VY];
        entry["sz"] = joint.getPublicScale().mV[VZ];
        result.append(entry);
    }
    return result;
}

void FSPosingMotion::fromLLSD(const LLSD& data)
{
    if (!data.isArray()) return;

    for (LLSD::array_const_iterator it = data.beginArray(); it != data.endArray(); ++it)
    {
        const LLSD& entry = *it;
        std::string name = entry["n"].asString();
        FSJointPose* pose = getJointPoseByJointName(name);
        if (pose)
        {
            LLQuaternion rot((F32)entry["rx"].asReal(), (F32)entry["ry"].asReal(),
                             (F32)entry["rz"].asReal(), (F32)entry["rw"].asReal());
            LLVector3 pos((F32)entry["px"].asReal(), (F32)entry["py"].asReal(), (F32)entry["pz"].asReal());
            LLVector3 scale((F32)entry["sx"].asReal(), (F32)entry["sy"].asReal(), (F32)entry["sz"].asReal());

            pose->setPublicRotation(false, false, POSER_CHANGE_ROTATION, rot);
            if (!pos.isExactlyZero()) pose->setPublicPosition(pos);
            if (!scale.isExactlyZero()) pose->setPublicScale(scale);
        }
    }
}
