/**
 * @file fsfloaterimcontainer.cpp
 * @brief Multifloater containing active IM sessions in separate tab container tabs
 *
 * $LicenseInfo:firstyear=2009&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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

// Original file: llimfloatercontainer.cpp


#include "llviewerprecompiledheaders.h"

#include "fsfloaterimcontainer.h"
#include "lldraghandle.h"

#include "fsfloatercontacts.h"
#include "fsfloaterim.h"
#include "fsfloaternearbychat.h"
#include "llfloaterreg.h"
#include "llchiclet.h"
#include "llchicletbar.h"
#include "llemojihelper.h"
#include "lltoolbarview.h"
#include "llurlregistry.h"
#include "llvoiceclient.h"
#include "fsnearbychathub.h"
#include "llagentui.h"
#include "llcallingcard.h"
#include "llmd5.h"
#include "llfilepicker.h"
#include "llviewermenufile.h"
#include "llbutton.h"
#include "lltexteditor.h"
#include "lltextbox.h"
#include "lllineeditor.h"
#include "llcheckboxctrl.h"
#include "llnotificationsutil.h"
#include "llcombobox.h"
#include "llscrolllistctrl.h"
#include "llscrolllistitem.h"
#include "llchat.h"
#include "llagent.h"
#include "llinventorymodel.h"
#include "llviewerinventory.h"
#include "llpreviewnotecard.h"
#include "llfoldertype.h"
#include "llinventorydefines.h"
#include "llfloaterperms.h"
#include <boost/json.hpp>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <thread>

// Defined in fsfloaterim.cpp — sends a slash command to the MCP server
extern void sendMCPRequest(const LLUUID& session_id, const std::string& text);

const LLUUID AI_AGENT_SESSION_ID("6a0f6a0f-6a0f-6a0f-6a0f-6a0f6a0f6a0f");
const LLUUID AI_AGENT_2_SESSION_ID("6a0f6a0f-6a0f-6a0f-6a0f-6a0f6a0f6a1f");

// ── Discord session registry ──────────────────────────────────────────────────
std::map<LLUUID, std::string> sDiscordSessions;
std::map<LLUUID, LLUUID>      sDiscordOriginalUUIDs;
std::map<LLUUID, std::string> sDiscordChannelIds;
std::mutex                    sDiscordMutex;

// Fire-and-forget POST to the Discord relay
void postToRelay(const std::string& path, const std::string& body)
{
    std::thread([path, body]()
    {
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return;
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(3002);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) { ::close(sock); return; }
        std::string req = "POST " + path + " HTTP/1.1\r\n"
                          "Host: 127.0.0.1:3002\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: " + std::to_string(body.size()) + "\r\n"
                          "Connection: close\r\n\r\n" + body;
        ::send(sock, req.c_str(), req.size(), 0);
        char buf[256]; while (::recv(sock, buf, sizeof(buf), 0) > 0) {}
        ::close(sock);
    }).detach();
}

LLUUID discordUUID(const std::string& discord_id)
{
    // Deterministic UUID from Discord user ID — must match discord_relay.py
    std::string key = "discord:" + discord_id;
    LLMD5 md5;
    md5.update((const U8*)key.c_str(), (U32)key.size());
    md5.finalize();
    LLUUID result;
    md5.raw_digest(result.mData);
    return result;
}

bool isDiscordSession(const LLUUID& session_id)
{
    std::lock_guard<std::mutex> lk(sDiscordMutex);
    return sDiscordSessions.count(session_id) > 0;
}

std::string getDiscordDisplayName(const LLUUID& session_id)
{
    std::lock_guard<std::mutex> lk(sDiscordMutex);
    auto it = sDiscordSessions.find(session_id);
    return (it != sDiscordSessions.end()) ? it->second : "Discord";
}

// <FS:PP> Restore open IMs from previous session
#include "llconversationlog.h"
#include "llimview.h"
// </FS:PP>

constexpr F32 VOICE_STATUS_UPDATE_INTERVAL = 1.0f;

//
// FSFloaterIMContainer
//
FSFloaterIMContainer::FSFloaterIMContainer(const LLSD& seed)
:   LLMultiFloater(seed),
    mActiveVoiceFloater(nullptr),
    mCurrentVoiceState(VOICE_STATE_NONE),
    mForceVoiceStateUpdate(false),
    mIsAddingNewSession(false)
{
    mAutoResize = false;
    LLTransientFloaterMgr::getInstance()->addControlView(LLTransientFloaterMgr::IM, this);

    // Firstly add our self to IMSession observers, so we catch session events
    LLIMMgr::getInstance()->addSessionObserver(this);
}

FSFloaterIMContainer::~FSFloaterIMContainer()
{
    mNewMessageConnection.disconnect();
    LLTransientFloaterMgr::getInstance()->removeControlView(LLTransientFloaterMgr::IM, this);

    if (LLIMMgr::instanceExists())
    {
        LLIMMgr::getInstance()->removeSessionObserver(this);
    }
}

bool FSFloaterIMContainer::postBuild()
{
    mNewMessageConnection = LLIMModel::instance().mNewMsgSignal.connect(boost::bind(&FSFloaterIMContainer::onNewMessageReceived, this, _1));
    // Do not call base postBuild to not connect to mCloseSignal to not close all floaters via Close button
    // mTabContainer will be initialized in LLMultiFloater::addChild()

    mTabContainer->setAllowRearrange(true);
    mTabContainer->setRearrangeCallback(boost::bind(&FSFloaterIMContainer::onIMTabRearrange, this, _1, _2));
    mTabContainer->setCommitCallback(boost::bind(&FSFloaterIMContainer::onTabSelectedDiscord, this));

    mActiveVoiceUpdateTimer.setTimerExpirySec(VOICE_STATUS_UPDATE_INTERVAL);
    mActiveVoiceUpdateTimer.start();

    gSavedSettings.getControl("FSShowConversationVoiceStateIndicator")->getSignal()->connect(boost::bind(&FSFloaterIMContainer::onVoiceStateIndicatorChanged, this, _2));

    return true;
}

void FSFloaterIMContainer::initTabs()
{
    // If we're using multitabs, and we open up for the first time
    // Add localchat by default if it's not already on the screen somewhere else. -AO
    // But only if it hasnt been already so we can reopen it to the same tab -KC
    // Improved handling to leave most of the work to the LL tear-off code -Zi
    // This is mirrored from FSFloaterContacts::onOpen() and FSFloaterNearbyChat::onOpen()
    // respectively: If those floaters are hosted, they don't store their visibility state.
    // Instead, the visibility state of the hosting container is stored. (See Zi's changes to
    // LLFloater::storeVisibilityControl()) That means if contacts and/or nearby chat floater
    // are hosted and FSFloaterIMContainer was visible at logout, we will end up here during
    // next login and have to configure those floaters so their tear off state and icon is
    // correct. Configure contacts first and nearby chat last so nearby chat will be active
    // once FSFloaterIMContainer has opened. -AH

    FSFloaterContacts* floater_contacts = FSFloaterContacts::getInstance();
    if (!LLFloater::isVisible(floater_contacts) && (floater_contacts->getHost() != this))
    {
        if (gSavedSettings.getBOOL("ContactsTornOff"))
        {
            // first set the tear-off host to the conversations container
            floater_contacts->setHost(this);
            // clear the tear-off host right after, the "last host used" will still stick
            floater_contacts->setHost(NULL);
            // reparent to floater view
            gFloaterView->addChild(floater_contacts);
        }
        else
        {
            addFloater(floater_contacts, true, IM_NOTHING_SPECIAL);
        }
    }

    LLFloater* floater_chat = FSFloaterNearbyChat::getInstance();
    if (!LLFloater::isVisible(floater_chat) && (floater_chat->getHost() != this))
    {
        if (gSavedSettings.getBOOL("ChatHistoryTornOff"))
        {
            // first set the tear-off host to this floater
            floater_chat->setHost(this);
            // clear the tear-off host right after, the "last host used" will still stick
            floater_chat->setHost(NULL);
            // reparent to floater view
            gFloaterView->addChild(floater_chat);
        }
        else
        {
            addFloater(floater_chat, true, IM_NOTHING_SPECIAL);
        }
    }

    const LLUUID agent_ids[] = { AI_AGENT_SESSION_ID, AI_AGENT_2_SESSION_ID };
    if (gIMMgr)
    {
        const std::string name_settings[] = { "FSAIAgentName1", "FSAIAgentName2" };
        const std::string default_names[] = { "AI Agent", "AI Agent 2" };

        for (int i = 0; i < 2; ++i)
        {
            const LLUUID& id = agent_ids[i];
            std::string name = default_names[i];
            if (gSavedSettings.controlExists(name_settings[i]))
            {
                name = gSavedSettings.getString(name_settings[i]);
            }

            if (!LLIMModel::instance().findIMSession(id))
            {
                LLIMModel::instance().newSession(id, name, IM_NOTHING_SPECIAL, LLUUID::null);

                // Inject avatar name into cache 
                LLAvatarName agent_name;
                agent_name.fromString(name);
                LLAvatarNameCache::instance().insert(id, agent_name);

                // Inject into buddy list (Contacts)
                LLAvatarTracker::buddy_map_t agent_buddy;
                LLRelationship* rel = new LLRelationship(LLRelationship::GRANT_ONLINE_STATUS, LLRelationship::GRANT_ONLINE_STATUS, true);
                agent_buddy[id] = rel;
                LLAvatarTracker::instance().addBuddyList(agent_buddy);
            }
            LLIMModel::LLIMSession* session = LLIMModel::instance().findIMSession(id);
            if (session)
            {
                session->mSessionInitialized = true;
                // Update name in case it changed in settings
                session->mName = name;
            }
        }
    }
}

// [SL:KB] - Patch: UI-TabRearrange | Checked: 2012-05-05 (Catznip-3.3.0)
void FSFloaterIMContainer::onIMTabRearrange(S32 tab_index, LLPanel* tab_panel)
{
    LLFloater* pIMFloater = dynamic_cast<LLFloater*>(tab_panel);
    if (!pIMFloater)
        return;

    const LLUUID& idSession = pIMFloater->getKey().asUUID();
    if (idSession.isNull())
        return;

    LLChicletPanel* pChicletPanel = LLChicletBar::instance().getChicletPanel();
    LLChiclet* pIMChiclet = pChicletPanel->findChiclet<LLChiclet>(idSession);
    pChicletPanel->setChicletIndex(pIMChiclet, tab_index - mTabContainer->getNumLockedTabs());
}
// [/SL:KB]

void FSFloaterIMContainer::onOpen(const LLSD& key)
{
    LLMultiFloater::onOpen(key);
    initTabs();
    LLFloater* active_floater = getActiveFloater();
    if (active_floater && !active_floater->hasFocus())
    {
        mTabContainer->setFocus(true);
    }
}

void FSFloaterIMContainer::onClose(bool app_quitting)
{
    if (app_quitting)
    {
        saveOpenIMs(); // <FS:PP> Save open IM sessions before closing
        for (S32 i = 0; i < mTabContainer->getTabCount(); ++i)
        {
            FSFloaterIM* floater = dynamic_cast<FSFloaterIM*>(mTabContainer->getPanelByIndex(i));
            if (floater)
            {
                floater->onClose(app_quitting);
            }
        }
    }
}

void FSFloaterIMContainer::addFloater(LLFloater* floaterp,
                                    bool select_added_floater,
                                    LLTabContainer::eInsertionPoint insertion_point)
{
    if (!floaterp)
    {
        return;
    }

    EInstantMessage type{ IM_NOTHING_SPECIAL };

    LLIMModel::LLIMSession* session = LLIMModel::getInstance()->findIMSession(floaterp->getKey());
    if (session)
    {
        type = session->mType;
    }

    addFloater(floaterp, select_added_floater, type, insertion_point);
}

void FSFloaterIMContainer::addFloater(LLFloater* floaterp,
                                    bool select_added_floater,
                                    EInstantMessage type,
                                    LLTabContainer::eInsertionPoint no_auto_insertion_point)
{
    if (!floaterp)
        return;

    // already here
    if (floaterp->getHost() == this)
    {
        openFloater(floaterp->getKey());
        return;
    }

    // Need to force an update on the voice state because torn off floater might get re-attached
    mForceVoiceStateUpdate = true;

    if (floaterp->getName() == "imcontacts" || floaterp->getName() == "nearby_chat")
    {
        S32 num_locked_tabs = mTabContainer->getNumLockedTabs();
        mTabContainer->unlockTabs();
        // add contacts window as first tab
        if (floaterp->getName() == "imcontacts")
        {
            LLMultiFloater::addFloater(floaterp, select_added_floater, LLTabContainer::START);
            gSavedSettings.setBOOL("ContactsTornOff", false);
        }
        else
        {
            // add chat history as second tab if contact window is present, first tab otherwise
            if (dynamic_cast<FSFloaterContacts*>(mTabContainer->getPanelByIndex(0)))
            {
                // assuming contacts window is first tab, select it
                mTabContainer->selectFirstTab();
                // and add ourselves after
                LLMultiFloater::addFloater(floaterp, select_added_floater, LLTabContainer::RIGHT_OF_CURRENT);
            }
            else
            {
                LLMultiFloater::addFloater(floaterp, select_added_floater, LLTabContainer::START);
            }
            gSavedSettings.setBOOL("ChatHistoryTornOff", false);
        }
        // make sure first two tabs are now locked
        mTabContainer->lockTabs(num_locked_tabs + 1);

        floaterp->setCanClose(false);
        return;
    }

    LLTabContainer::eInsertionPoint insertion_point =  no_auto_insertion_point;

    if (mIsAddingNewSession)
    {
        if (gSavedSettings.getBOOL("FSAutoOrderIMTabs"))
        {
            const std::string order_priorities = gSavedSettings.getString("FSAutoOrderIMTabsPriorities");

            // sort order uses the ascii values for 0, 1 and 2, just to make it simpler to store as a string
            // and the values can be easily compared via greater or less than
            std::map<EInstantMessage, S32> sort_order;
            sort_order[IM_SESSION_GROUP_START] = order_priorities[0];
            sort_order[IM_SESSION_CONFERENCE_START] =  order_priorities[1];
            sort_order[IM_NOTHING_SPECIAL] = order_priorities[2];

            bool insert_at_top = gSavedSettings.getBOOL("FSAutoOrderIMTabsAtTop");

            if (type == IM_SESSION_INVITE)
            {
                // make sure group IMs are always the same value
                type = IM_SESSION_GROUP_START;
            }
            bool type_found{ false };

            S32 index;
            for (index = mTabContainer->getNumLockedTabs(); index < mTabContainer->getTabCount(); index++)
            {
                FSFloaterIM* tab = dynamic_cast<FSFloaterIM*>(mTabContainer->getPanelByIndex(index));
                if (!tab)
                {
                    continue;
                }

                const LLIMModel::LLIMSession* session = LLIMModel::getInstance()->findIMSession(tab->getKey());
                if (!session)
                {
                    continue;
                }

                EInstantMessage session_type = session->mType;
                if (session_type == IM_SESSION_INVITE)
                {
                    // make sure group IMs are always the same value
                    session_type = IM_SESSION_GROUP_START;
                }

                if (session_type == type)
                {
                    if (insert_at_top)
                    {
                        break;
                    }
                    type_found = true;
                }
                else if (type_found)
                {
                    break;
                }
                else if (sort_order[session_type] < sort_order[type])
                {
                    break;
                }
            }

            insertion_point = (LLTabContainer::eInsertionPoint)index;
        }
    }
    else
    {
// [SL:KB] - Patch: UI-TabRearrange | Checked: 2012-06-22 (Catznip-3.3.0)
        // If we're redocking a torn off IM floater, return it back to its previous place
        if (floaterp->isTornOff())
        {
            LLChicletPanel* pChicletPanel = LLChicletBar::instance().getChicletPanel();

            LLIMChiclet* pChiclet = pChicletPanel->findChiclet<LLIMChiclet>(floaterp->getKey());
            if (pChiclet)
            {
                S32 idxChiclet = pChicletPanel->getChicletIndex(pChiclet);
                if ((idxChiclet > 0) && (idxChiclet < pChicletPanel->getChicletCount()))
                {
                    // Look for the first IM session to the left of this one
                    while (--idxChiclet >= 0)
                    {
                        if ((pChiclet = dynamic_cast<LLIMChiclet*>(pChicletPanel->getChiclet(idxChiclet))))
                        {
                            FSFloaterIM* pFloater = FSFloaterIM::findInstance(pChiclet->getSessionId());
                            if (pFloater)
                            {
                                insertion_point = (LLTabContainer::eInsertionPoint)(mTabContainer->getIndexForPanel(pFloater) + 1);
                                break;
                            }
                        }
                    }
                }
                else
                {
                    insertion_point = (0 == idxChiclet) ? LLTabContainer::START : LLTabContainer::END;
                }
            }
        }
// [/SL:KB]
    }

    LLFloater* active_floater = getActiveFloater();

    LLMultiFloater::addFloater(floaterp, select_added_floater, insertion_point);

    if (!select_added_floater && active_floater)
    {
        selectFloater(active_floater);
    }

    LLUUID session_id = floaterp->getKey();
    mSessions[session_id] = floaterp;
    floaterp->mCloseSignal.connect(boost::bind(&FSFloaterIMContainer::onCloseFloater, this, session_id));
}

void FSFloaterIMContainer::addNewSession(LLFloater* floaterp, EInstantMessage type)
{
    // Make sure we don't do some strange re-arranging if we add a new IM floater due to a new session
    mIsAddingNewSession = true;
    addFloater(floaterp, false, type);
    mIsAddingNewSession = false;
}

// [SL:KB] - Patch: Chat-NearbyChatBar | Checked: 2011-12-11 (Catznip-3.2.0d) | Added: Catznip-3.2.0d
void FSFloaterIMContainer::removeFloater(LLFloater* floaterp)
{
    const std::string floater_name = floaterp->getName();
    std::string setting_name = "";
    bool needs_unlock = false;
    if (floater_name == "nearby_chat")
    {
        setting_name = "ChatHistoryTornOff";
        needs_unlock = true;
    }
    else if (floater_name == "imcontacts")
    {
        setting_name = "ContactsTornOff";
        needs_unlock = true;
    }

    if (needs_unlock)
    {
        // Calling lockTabs with 0 will lock ALL tabs - need to call unlockTabs instead!
        S32 num_locked_tabs = mTabContainer->getNumLockedTabs();
        if (num_locked_tabs > 1)
        {
            mTabContainer->lockTabs(num_locked_tabs - 1);
        }
        else
        {
            mTabContainer->unlockTabs();
        }
        gSavedSettings.setBOOL(setting_name, true);
        floaterp->setCanClose(true);
    }

    mFlashingTabStates.erase(floaterp);

    LLMultiFloater::removeFloater(floaterp);
}
// [/SL:KB]

bool FSFloaterIMContainer::hasFloater(LLFloater* floaterp)
{
    for (S32 i = 0; i < mTabContainer->getTabCount(); ++i)
    {
        if (dynamic_cast<LLFloater*>(mTabContainer->getPanelByIndex(i)) == floaterp)
        {
            return true;
        }
    }
    return false;
}

void FSFloaterIMContainer::onCloseFloater(LLUUID& id)
{
    LLFloater* session_floater = mSessions[id];
    bool was_session_shown = session_floater && session_floater->isShown();

    mSessions.erase(id);
    if (was_session_shown && isShown())
    {
        setFocus(true);
    }
    else if (isMinimized())
    {
        setMinimized(true); // Make sure console output that needs to be shown is still doing so
    }
}

void FSFloaterIMContainer::onNewMessageReceived(const LLSD& msg)
{
    LLUUID session_id = msg["session_id"].asUUID();
    LLFloater* floaterp = get_ptr_in_map(mSessions, session_id);
    LLFloater* current_floater = LLMultiFloater::getActiveFloater();

    // KC: Don't flash tab on friend status changes per setting
    if (floaterp && current_floater && floaterp != current_floater
        && (gSavedSettings.getBOOL("FSIMChatFlashOnFriendStatusChange") || !msg.has("from_id") || msg["from_id"].asUUID().notNull()))
    {
        startFlashingTab(floaterp, msg["message"].asString());
    }
}

FSFloaterIMContainer* FSFloaterIMContainer::findInstance()
{
    return LLFloaterReg::findTypedInstance<FSFloaterIMContainer>("fs_im_container");
}

FSFloaterIMContainer* FSFloaterIMContainer::getInstance()
{
    return LLFloaterReg::getTypedInstance<FSFloaterIMContainer>("fs_im_container");
}

// <FS:TJ> [FIRE-35804] Allow the IM floater to have separate transparency
F32 FSFloaterIMContainer::getCurrentTransparency()
{
    static LLCachedControl<F32> im_opacity(gSavedSettings, "FSIMOpacity", 1.0f);
    static LLCachedControl<bool> im_active_opacity_override(gSavedSettings, "FSImActiveOpacityOverride", false);

    F32 floater_opacity = LLUICtrl::getCurrentTransparency();
    if (im_active_opacity_override && getTransparencyType() == TT_ACTIVE)
    {
        return floater_opacity;
    }

    return llmin(im_opacity(), floater_opacity);
}
// </FS:TJ>

void FSFloaterIMContainer::setVisible(bool b)
{
    LLMultiFloater::setVisible(b);

    if (b)
    {
        mFlashingSessions.clear();
    }
}

void FSFloaterIMContainer::setMinimized(bool b)
{
    if (mTabContainer)
    {
        if (FSFloaterNearbyChat* nearby_floater = dynamic_cast<FSFloaterNearbyChat*>(mTabContainer->getCurrentPanel()); nearby_floater)
        {
            nearby_floater->handleMinimized(b);
        }

        if (FSFloaterIM* im_floater = dynamic_cast<FSFloaterIM*>(mTabContainer->getCurrentPanel()); im_floater)
        {
            im_floater->handleMinimized(b);
        }
    }

    LLMultiFloater::setMinimized(b);
}

//virtual
void FSFloaterIMContainer::sessionAdded(const LLUUID& session_id, const std::string& name, const LLUUID& other_participant_id, bool has_offline_msg)
{
    LLIMModel::LLIMSession* session = LLIMModel::getInstance()->findIMSession(session_id);
    if (!session)
        return;

    FSFloaterIM::onNewIMReceived(session_id);
}

//virtual
void FSFloaterIMContainer::sessionRemoved(const LLUUID& session_id)
{
    if (FSFloaterIM* iMfloater = LLFloaterReg::findTypedInstance<FSFloaterIM>("fs_impanel", session_id); iMfloater)
    {
        iMfloater->closeFloater();
    }

    // If this was a temporarily-enabled Discord channel, disable it on the relay
    {
        std::lock_guard<std::mutex> lk(sDiscordMutex);
        auto it = sDiscordChannelIds.find(session_id);
        if (it != sDiscordChannelIds.end())
        {
            std::string cid = it->second;
            sDiscordChannelIds.erase(it);
            postToRelay("/channel_active", "{\"channel_id\":" + cid + ",\"active\":false}");
        }
    }

    uuid_vec_t::iterator found = std::find(mFlashingSessions.begin(), mFlashingSessions.end(), session_id);
    if (found != mFlashingSessions.end())
    {
        mFlashingSessions.erase(found);
        checkFlashing();
    }
}

// static
void FSFloaterIMContainer::reloadEmptyFloaters()
{
    LLFloaterReg::const_instance_list_t& inst_list = LLFloaterReg::getFloaterList("fs_impanel");
    for (LLFloaterReg::const_instance_list_t::const_iterator iter = inst_list.begin();
        iter != inst_list.end(); ++iter)
    {
        FSFloaterIM* floater = dynamic_cast<FSFloaterIM*>(*iter);
        if (floater && floater->getLastChatMessageIndex() == -1)
        {
            floater->reloadMessages(true);
        }
    }

    FSFloaterNearbyChat* nearby_chat = LLFloaterReg::findTypedInstance<FSFloaterNearbyChat>("fs_nearby_chat");
    if (nearby_chat && nearby_chat->getMessageArchiveLength() == 0)
    {
        nearby_chat->reloadMessages(true);
    }
}

void FSFloaterIMContainer::onVoiceStateIndicatorChanged(const LLSD& data)
{
    if (!data.asBoolean())
    {
        if (mActiveVoiceFloater)
        {
            mTabContainer->setTabImage(mActiveVoiceFloater, "");
            mActiveVoiceFloater = NULL;
        }
        mCurrentVoiceState = VOICE_STATE_NONE;
    }
}

// virtual
void FSFloaterIMContainer::draw()
{
    static LLCachedControl<bool> fsShowConversationVoiceStateIndicator(gSavedSettings, "FSShowConversationVoiceStateIndicator");
    if (fsShowConversationVoiceStateIndicator && (mActiveVoiceUpdateTimer.hasExpired() || mForceVoiceStateUpdate))
    {
        LLFloater* current_voice_floater = getCurrentVoiceFloater();
        if (mActiveVoiceFloater != current_voice_floater)
        {
            if (mActiveVoiceFloater)
            {
                mTabContainer->setTabImage(mActiveVoiceFloater, "");
                mCurrentVoiceState = VOICE_STATE_NONE;
            }
        }

        if (current_voice_floater)
        {
            static LLUIColor voice_connected_color = LLUIColorTable::instance().getColor("VoiceConnectedColor", LLColor4::green);
            static LLUIColor voice_error_color = LLUIColorTable::instance().getColor("VoiceErrorColor", LLColor4::red);
            static LLUIColor voice_not_connected_color = LLUIColorTable::instance().getColor("VoiceNotConnectedColor", LLColor4::yellow);

            eVoiceState voice_state = VOICE_STATE_UNKNOWN;
            LLVoiceChannel* voice_channel = LLVoiceChannel::getCurrentVoiceChannel();
            if (voice_channel)
            {
                if (voice_channel->isActive())
                {
                    voice_state = VOICE_STATE_CONNECTED;
                }
                else if (voice_channel->getState() == LLVoiceChannel::STATE_ERROR)
                {
                    voice_state = VOICE_STATE_ERROR;
                }
                else
                {
                    voice_state = VOICE_STATE_NOT_CONNECTED;
                }
            }

            if (voice_state != mCurrentVoiceState || mForceVoiceStateUpdate)
            {
                LLColor4 icon_color;
                switch (voice_state)
                {
                    case VOICE_STATE_CONNECTED:
                        icon_color = voice_connected_color.get();
                        break;
                    case VOICE_STATE_ERROR:
                        icon_color = voice_error_color.get();
                        break;
                    case VOICE_STATE_NOT_CONNECTED:
                        icon_color = voice_not_connected_color.get();
                        break;
                    default:
                        icon_color = LLColor4::white;
                        break;
                }
                mTabContainer->setTabImage(current_voice_floater, "Active_Voice_Tab", LLFontGL::RIGHT, icon_color, icon_color);
                mCurrentVoiceState = voice_state;
            }
        }
        mForceVoiceStateUpdate = false;
        mActiveVoiceFloater = current_voice_floater;
        mActiveVoiceUpdateTimer.setTimerExpirySec(VOICE_STATUS_UPDATE_INTERVAL);
    }

    LLMultiFloater::draw();
}

LLFloater* FSFloaterIMContainer::getCurrentVoiceFloater()
{
    if (!LLVoiceClient::instance().voiceEnabled())
    {
        return nullptr;
    }

    if (LLVoiceChannelProximal::getInstance() == LLVoiceChannel::getCurrentVoiceChannel())
    {
        return FSFloaterNearbyChat::getInstance();
    }

    for (S32 i = 0; i < mTabContainer->getTabCount(); ++i)
    {
        FSFloaterIM* im_floater = dynamic_cast<FSFloaterIM*>(mTabContainer->getPanelByIndex(i));
        if (im_floater && im_floater->getVoiceChannel() == LLVoiceChannel::getCurrentVoiceChannel())
        {
            return im_floater;
        }
    }
    return nullptr;
}

void FSFloaterIMContainer::addFlashingSession(const LLUUID& session_id)
{
    uuid_vec_t::iterator found = std::find(mFlashingSessions.begin(), mFlashingSessions.end(), session_id);
    if (found == mFlashingSessions.end())
    {
        mFlashingSessions.emplace_back(session_id);
    }

    checkFlashing();
}

void FSFloaterIMContainer::checkFlashing()
{
    gToolBarView->flashCommand(LLCommandId("chat"), !mFlashingSessions.empty(), isMinimized());
}

void FSFloaterIMContainer::sessionIDUpdated(const LLUUID& old_session_id, const LLUUID& new_session_id)
{
    if (avatarID_panel_map_t::iterator found = mSessions.find(old_session_id); found != mSessions.end())
    {
        LLFloater* floaterp = found->second;
        mSessions.erase(found);
        mSessions[new_session_id] = floaterp;
    }
}

void FSFloaterIMContainer::tabOpen(LLFloater* opened_floater, bool from_click)
{
    LLEmojiHelper::instance().hideHelper(nullptr, true);

    mFlashingTabStates.erase(opened_floater);

    // For Discord sessions, refresh the floater title from sDiscordSessions now,
    // before LLMultiFloater::onTabSelected() calls mDragHandle->setTitle().
    FSFloaterIM* im_floater = dynamic_cast<FSFloaterIM*>(opened_floater);
    if (im_floater)
    {
        LLUUID session_id = opened_floater->getKey().asUUID();
        std::string discord_name;
        {
            std::lock_guard<std::mutex> lk(sDiscordMutex);
            auto it = sDiscordSessions.find(session_id);
            if (it != sDiscordSessions.end())
                discord_name = it->second;
        }
        if (!discord_name.empty())
            im_floater->updateSessionName(discord_name, discord_name);
    }
}

void FSFloaterIMContainer::onTabSelectedDiscord()
{
    LLMultiFloater::onTabSelected();

    // Force the correct title for Discord sessions — overrides whatever onTabSelected set.
    LLFloater* floaterp = dynamic_cast<LLFloater*>(mTabContainer->getCurrentPanel());
    if (!floaterp) return;
    LLUUID sid = floaterp->getKey().asUUID();
    if (!isDiscordSession(sid)) return;

    std::string name = getDiscordDisplayName(sid);
    // Strip " (discord)" suffix — channels keep "#ch / Server", DMs become "Name / discord"
    static const std::string kSuffix = " (discord)";
    if (name.size() > kSuffix.size() &&
        name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0)
    {
        std::string base = name.substr(0, name.size() - kSuffix.size());
        name = (base.rfind("#", 0) == 0) ? base : base + " / discord";
    }

    LL_INFOS("Discord") << "[DISCORD-TITLE-DBG] onTabSelectedDiscord: name='" << name
                        << "' floater title='" << floaterp->getTitle()
                        << "' floater shortTitle='" << floaterp->getShortTitle() << "'" << LL_ENDL;

    // Force ALL title surfaces at once
    floaterp->setTitle(name);
    floaterp->setShortTitle(name);
    S32 idx = mTabContainer->getIndexForPanel(floaterp);
    if (idx != -1) mTabContainer->setPanelTitle(idx, name);
    getDragHandle()->setTitle(getTitle() + " - " + name);
}

void FSFloaterIMContainer::startFlashingTab(LLFloater* floater, const std::string& message)
{
    const bool contains_mention = LLUrlRegistry::getInstance()->containsAgentMention(message);

    auto& [session_floater, is_alt_flashing] = *(mFlashingTabStates.try_emplace(floater, false).first);
    is_alt_flashing = is_alt_flashing || contains_mention;

    if (LLMultiFloater::isFloaterFlashing(floater))
    {
        LLMultiFloater::setFloaterFlashing(floater, false);
    }
    LLMultiFloater::setFloaterFlashing(floater, true, is_alt_flashing);
}

// <FS:PP> Restore open IMs from previous session
void FSFloaterIMContainer::saveOpenIMs()
{
    if (!gSavedSettings.getBOOL("FSRestoreOpenIMs"))
    {
        gSavedPerAccountSettings.setLLSD("FSLastOpenIMs", LLSD::emptyArray());
        return;
    }

    LLSD openIMs = LLSD::emptyArray();
    for (S32 i = 0; i < mTabContainer->getTabCount(); ++i)
    {
        FSFloaterIM* floater = dynamic_cast<FSFloaterIM*>(mTabContainer->getPanelByIndex(i));
        if (floater)
        {
            LLUUID session_id = floater->getKey();
            if (session_id.notNull())
            {
                LLIMModel::LLIMSession* session = LLIMModel::getInstance()->findIMSession(session_id);
                if (session && session->mSessionType == LLIMModel::LLIMSession::P2P_SESSION
                    && !isDiscordSession(session_id)
                    && session->mName.find("(discord)") == std::string::npos
                    && session_id != AI_AGENT_SESSION_ID
                    && session_id != AI_AGENT_2_SESSION_ID)
                {
                    LLSD session_data = LLSD::emptyMap();
                    session_data["other_participant_id"] = session->mOtherParticipantID;
                    session_data["session_name"] = session->mName;
                    openIMs.append(session_data);
                }
            }
        }
    }

    gSavedPerAccountSettings.setLLSD("FSLastOpenIMs", openIMs);
}

void FSFloaterIMContainer::restoreOpenIMs()
{
    LLSD openIMs = gSavedPerAccountSettings.getLLSD("FSLastOpenIMs");
    if (!openIMs.isArray() || openIMs.size() == 0)
    {
        return;
    }

    for (LLSD::array_const_iterator it = openIMs.beginArray(); it != openIMs.endArray(); ++it)
    {
        LLSD session_data = *it;
        if (session_data.isMap())
        {
            LLUUID other_participant_id = session_data["other_participant_id"].asUUID();
            std::string session_name = session_data["session_name"].asString();
            // Skip Discord sessions that were incorrectly saved — they will be
            // re-created when the SSE relay delivers the first message.
            bool is_discord = session_name.find("(discord)") != std::string::npos;
            bool is_ai = (other_participant_id == AI_AGENT_SESSION_ID || other_participant_id == AI_AGENT_2_SESSION_ID);
            LL_INFOS("Discord") << "[DISCORD-TITLE-DBG] restoreOpenIMs: session_name='" << session_name
                                << "' is_discord=" << is_discord << " is_ai=" << is_ai << LL_ENDL;
            if (other_participant_id.notNull() && !is_discord && !is_ai)
            {
                LLUUID new_session_id;
                new_session_id = LLIMMgr::getInstance()->addSession(session_name, IM_NOTHING_SPECIAL, other_participant_id);
                if (new_session_id.notNull())
                {
                    FSFloaterIM* im_floater = FSFloaterIM::show(new_session_id);
                    if (im_floater)
                    {
                        if (im_floater->getHost() != this)
                        {
                            addFloater(im_floater, false, IM_NOTHING_SPECIAL);
                        }
                    }
                }
            }
        }
    }
}
// </FS:PP>

// static
FSFloaterAIAgent::FSFloaterAIAgent(const LLUUID& session_id)
  : FSFloaterIM(session_id)
{
	setIsSingleInstance(false);
}

bool FSFloaterAIAgent::postBuild()
{
	bool result = FSFloaterIM::postBuild();
	
	std::string name = (mSessionID == AI_AGENT_2_SESSION_ID) ? "AI Agent 2" : "AI Agent";
	if (mSessionID == AI_AGENT_SESSION_ID && gSavedSettings.controlExists("FSAIAgentName1"))
		name = gSavedSettings.getString("FSAIAgentName1");
	else if (mSessionID == AI_AGENT_2_SESSION_ID && gSavedSettings.controlExists("FSAIAgentName2"))
		name = gSavedSettings.getString("FSAIAgentName2");

	setTitle(name);
	return result;
}

FSFloaterAIAgent* FSFloaterAIAgent::getInstance(const LLUUID& session_id)
{
	return LLFloaterReg::getTypedInstance<FSFloaterAIAgent>("panel_ai_agent", session_id);
}

void FSFloaterAIAgent::draw()
{
    FSFloaterIM::draw();

    // Track which AI agent is currently being drawn so onOpen knows which session to edit
    FSFloaterAIConfig::sCurrentEditingSession = mSessionID;

}

// ── AI History notecard ───────────────────────────────────────────────────────

void aiCreateHistoryNotecard(const LLUUID& session_id,
                             const std::vector<std::pair<std::string,std::string>>& history)
{
    // Build the notecard text
    std::string agent_name = (session_id == AI_AGENT_2_SESSION_ID) ? "AI Agent 2" : "AI Agent";
    if (session_id == AI_AGENT_SESSION_ID && gSavedSettings.controlExists("FSAIAgentName1"))
        agent_name = gSavedSettings.getString("FSAIAgentName1");
    else if (session_id == AI_AGENT_2_SESSION_ID && gSavedSettings.controlExists("FSAIAgentName2"))
        agent_name = gSavedSettings.getString("FSAIAgentName2");

    std::string user_name = "You";
    LLAgentUI::buildFullname(user_name);

    std::ostringstream oss;
    oss << "=== " << agent_name << " — Conversation History ===\n\n";
    for (auto& [role, content] : history)
    {
        const std::string& speaker = (role == "assistant") ? agent_name : user_name;
        oss << "[" << speaker << "]\n" << content << "\n\n";
    }
    const std::string text = oss.str();

    // Determine parent folder (Notecards category)
    const LLUUID parent_id = gInventory.findCategoryUUIDForType(LLFolderType::FT_NOTECARD);

    // Notecard title = agent name + " History"
    const std::string title = agent_name + " History";

    // Create the inventory item; in the callback, open the preview and set text
    create_inventory_item(
        gAgent.getID(),
        gAgent.getSessionID(),
        parent_id,
        LLTransactionID::tnull,
        title,
        "AI conversation history",
        LLAssetType::AT_NOTECARD,
        LLInventoryType::IT_NOTECARD,
        NO_INV_SUBTYPE,
        LLFloaterPerms::getNextOwnerPerms("Notecards"),
        new LLBoostFuncInventoryCallback([text](const LLUUID& item_id)
        {
            if (item_id.isNull()) return;

            gInventory.updateItem(gInventory.getItem(item_id));
            gInventory.notifyObservers();

            // Open the notecard editor
            LLPreviewNotecard* preview =
                LLFloaterReg::showTypedInstance<LLPreviewNotecard>(
                    "preview_notecard", LLSD(item_id), TAKE_FOCUS_YES);
            if (!preview) return;

            LLTextEditor* editor = preview->getEditor();
            if (editor)
            {
                editor->setText(text);
                editor->insertText(""); // touch to mark dirty so Save button activates
            }
        }));
}

// ── FSFloaterAIModelList ──────────────────────────────────────────────────────

FSFloaterAIModelList::FSFloaterAIModelList(const LLSD& key)
    : LLFloater(key)
{
}

bool FSFloaterAIModelList::postBuild()
{
    getChild<LLButton>("ai_model_ok_btn")->setCommitCallback(
        boost::bind(&FSFloaterAIModelList::onOKClicked, this));
    getChild<LLButton>("ai_model_apply_btn")->setCommitCallback(
        boost::bind(&FSFloaterAIModelList::onApplyClicked, this));
    getChild<LLButton>("ai_model_quit_btn")->setCommitCallback(
        boost::bind(&FSFloaterAIModelList::onQuitClicked, this));

    // Context slider: update label live, persist to setting
    getChild<LLUICtrl>("ai_context_slider")->setCommitCallback(
        [this](LLUICtrl* ctrl, const LLSD&)
        {
            S32 k = (S32)ctrl->getValue().asReal();
            gSavedSettings.setS32("FSAIContextSizeK", k);
            std::string label = (k == 0) ? "auto" : std::to_string(k) + "k";
            getChild<LLTextBox>("ai_context_value")->setText(label);
        });

    return LLFloater::postBuild();
}

void FSFloaterAIModelList::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    mSessionID = FSFloaterAIConfig::sCurrentEditingSession;
    if (mSessionID.isNull())
        mSessionID = AI_AGENT_SESSION_ID;

    mModels.clear();
    getChild<LLScrollListCtrl>("model_list")->clearRows();
    sendMCPRequest(mSessionID, "/model list json");

    // Restore slider from persistent setting
    S32 ctx_k = gSavedSettings.getS32("FSAIContextSizeK");
    getChild<LLUICtrl>("ai_context_slider")->setValue(LLSD((F32)ctx_k));
    std::string ctx_label = (ctx_k == 0) ? "auto" : std::to_string(ctx_k) + "k";
    getChild<LLTextBox>("ai_context_value")->setText(ctx_label);
}

void FSFloaterAIModelList::renderModelList()
{
    LLScrollListCtrl* list = getChild<LLScrollListCtrl>("model_list");
    list->clearRows();

    for (auto& [manager, name, endpoint, active] : mModels)
    {
        std::string style = active ? "BOLD" : "NORMAL";
        LLSD row;
        row["columns"][0]["column"]     = "col_active";
        row["columns"][0]["value"]      = active ? "\xe2\x97\x8f" : "";  // UTF-8 ●
        row["columns"][0]["font-style"] = style;
        row["columns"][1]["column"]     = "col_type";
        row["columns"][1]["value"]      = manager;
        row["columns"][1]["font-style"] = style;
        row["columns"][2]["column"]     = "col_name";
        row["columns"][2]["value"]      = name;
        row["columns"][2]["font-style"] = style;
        row["columns"][3]["column"]     = "col_endpoint";
        row["columns"][3]["value"]      = endpoint;
        row["columns"][3]["font-style"] = style;
        list->addElement(row);
    }
}

/*static*/
void FSFloaterAIModelList::updateModels(
    const std::vector<std::tuple<std::string, std::string, std::string, bool>>& models)
{
    FSFloaterAIModelList* inst =
        LLFloaterReg::findTypedInstance<FSFloaterAIModelList>("fs_ai_model_list");
    if (!inst || !inst->isInVisibleChain()) return;

    inst->mModels = models;
    inst->renderModelList();
}

void FSFloaterAIModelList::notifyAgents(const std::string& type, const std::string& name)
{
    std::string msg = "Loading model **" + name + "** (" + type + ")...";
    std::string sender1 = "AI Agent";
    if (gSavedSettings.controlExists("FSAIAgentName1"))
        sender1 = gSavedSettings.getString("FSAIAgentName1");
    std::string sender2 = "AI Agent 2";
    if (gSavedSettings.controlExists("FSAIAgentName2"))
        sender2 = gSavedSettings.getString("FSAIAgentName2");
    LLIMModel::instance().addMessage(AI_AGENT_SESSION_ID,   sender1, AI_AGENT_SESSION_ID,   msg);
    LLIMModel::instance().addMessage(AI_AGENT_2_SESSION_ID, sender2, AI_AGENT_2_SESSION_ID, msg);
}

void FSFloaterAIModelList::onApplyClicked()
{
    // Always push context size to both agents, regardless of model selection
    S32 ctx_k = gSavedSettings.getS32("FSAIContextSizeK");
    std::string ctx_cmd = "/config context " + std::to_string(ctx_k);
    sendMCPRequest(AI_AGENT_SESSION_ID,   ctx_cmd);
    sendMCPRequest(AI_AGENT_2_SESSION_ID, ctx_cmd);

    LLScrollListCtrl* list = getChild<LLScrollListCtrl>("model_list");
    LLScrollListItem* item = list->getFirstSelected();
    if (!item) return;

    std::string type     = item->getColumn(1)->getValue().asString();
    std::string name     = item->getColumn(2)->getValue().asString();
    std::string endpoint = item->getColumn(3)->getValue().asString();

    // Optimistically mark the applied model as active in our local copy
    for (auto& [manager, mname, ep, active] : mModels)
        active = (manager == type && mname == name);

    renderModelList();
    notifyAgents(type, name);

    std::string cmd;
    if (type == "MLX")
        cmd = "/model load mlx " + name;
    else if (type == "LlamaCpp")
    {
        auto colon = endpoint.rfind(':');
        std::string host = (colon != std::string::npos) ? endpoint.substr(0, colon) : "127.0.0.1";
        std::string port = (colon != std::string::npos) ? endpoint.substr(colon + 1) : "8080";
        cmd = "/model load llamacpp " + name + " --host " + host + " --port " + port;
    }
    else
        cmd = "/model load ollama " + name;
    sendMCPRequest(mSessionID, cmd);
}

void FSFloaterAIModelList::onOKClicked()
{
    // Always push context size to both agents, regardless of model selection
    S32 ctx_k = gSavedSettings.getS32("FSAIContextSizeK");
    std::string ctx_cmd = "/config context " + std::to_string(ctx_k);
    sendMCPRequest(AI_AGENT_SESSION_ID,   ctx_cmd);
    sendMCPRequest(AI_AGENT_2_SESSION_ID, ctx_cmd);

    LLScrollListCtrl* list = getChild<LLScrollListCtrl>("model_list");
    LLScrollListItem* item = list->getFirstSelected();
    if (!item) { closeFloater(); return; }

    std::string type     = item->getColumn(1)->getValue().asString();
    std::string name     = item->getColumn(2)->getValue().asString();
    std::string endpoint = item->getColumn(3)->getValue().asString();

    notifyAgents(type, name);

    std::string cmd;
    if (type == "MLX")
        cmd = "/model load mlx " + name;
    else if (type == "LlamaCpp")
    {
        auto colon = endpoint.rfind(':');
        std::string host = (colon != std::string::npos) ? endpoint.substr(0, colon) : "127.0.0.1";
        std::string port = (colon != std::string::npos) ? endpoint.substr(colon + 1) : "8080";
        cmd = "/model load llamacpp " + name + " --host " + host + " --port " + port;
    }
    else
        cmd = "/model load ollama " + name;
    sendMCPRequest(mSessionID, cmd);
    closeFloater();
}

void FSFloaterAIModelList::onQuitClicked()
{
    closeFloater();
}

// ── FSFloaterAIModelConfig ─────────────────────────────────────────────────────

static const char*  kModelConfigSetting = "FSModelProviderConfigs";

FSFloaterAIModelConfig::FSFloaterAIModelConfig(const LLSD& key)
    : LLFloater(key)
{
}

bool FSFloaterAIModelConfig::postBuild()
{
    getChild<LLUICtrl>("model_config_list")->setCommitCallback(
        boost::bind(&FSFloaterAIModelConfig::onSelectionChanged, this));

    getChild<LLButton>("mc_add_btn")->setCommitCallback(
        boost::bind(&FSFloaterAIModelConfig::onAdd, this));
    getChild<LLButton>("mc_trash_btn")->setCommitCallback(
        boost::bind(&FSFloaterAIModelConfig::onTrash, this));
    getChild<LLButton>("mc_apply_btn")->setCommitCallback(
        boost::bind(&FSFloaterAIModelConfig::onApply, this));
    getChild<LLButton>("mc_save_btn")->setCommitCallback(
        boost::bind(&FSFloaterAIModelConfig::onSave, this));
    getChild<LLUICtrl>("llm_type")->setCommitCallback(
        boost::bind(&FSFloaterAIModelConfig::onTypeChanged, this));

    getChild<LLButton>("mc_close_btn")->setCommitCallback(
        boost::bind(&FSFloaterAIModelConfig::onCloseClicked, this));

    return LLFloater::postBuild();
}

void FSFloaterAIModelConfig::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    mConfigs = loadConfigs();
    renderList();
}

// ── Render the scroll list from mConfigs ──────────────────────────────────────

void FSFloaterAIModelConfig::renderList()
{
    LLScrollListCtrl* list = getChild<LLScrollListCtrl>("model_config_list");
    list->clearRows();

    for (const auto& cfg : mConfigs)
    {
        std::string style = cfg.is_current ? "BOLD" : "NORMAL";
        LLSD row;
        row["columns"][0]["column"]     = "mc_col_active";
        row["columns"][0]["value"]      = cfg.is_current ? "\xe2\x97\x8f" : "";
        row["columns"][0]["font-style"] = style;
        row["columns"][1]["column"]     = "mc_col_type";
        row["columns"][1]["value"]      = cfg.type;
        row["columns"][1]["font-style"] = style;
        row["columns"][2]["column"]     = "mc_col_model";
        row["columns"][2]["value"]      = cfg.model;
        row["columns"][2]["font-style"] = style;
        row["columns"][3]["column"]     = "mc_col_endpoint";
        row["columns"][3]["value"]      = cfg.endpoint;
        row["columns"][3]["font-style"] = style;
        row["value"] = cfg.id;
        list->addElement(row);
    }
}

// ── Selection changed: populate edit fields from selected config ─────────────

void FSFloaterAIModelConfig::onSelectionChanged()
{
    LLScrollListCtrl* list = getChild<LLScrollListCtrl>("model_config_list");
    LLScrollListItem* item = list->getFirstSelected();
    if (!item) return;

    std::string sel_id = item->getValue().asString();
    for (const auto& cfg : mConfigs)
    {
        if (cfg.id == sel_id)
        {
            getChild<LLUICtrl>("llm_type")->setValue(LLSD(cfg.type));
            getChild<LLUICtrl>("llm_model")->setValue(LLSD(cfg.model));
            getChild<LLUICtrl>("llm_endpoint")->setValue(LLSD(cfg.endpoint));
            getChild<LLUICtrl>("llm_apikey")->setValue(LLSD(cfg.apikey));
            getChild<LLUICtrl>("llm_context_length")->setValue(LLSD((F32)cfg.context_length));
            setEndpointAndApiKey(cfg.type);
            break;
        }
    }
}

// ── Add new model configuration ──────────────────────────────────────────────

void FSFloaterAIModelConfig::onAdd()
{
    std::string type = getChild<LLUICtrl>("llm_type")->getValue().asString();
    if (type.empty()) return;

    AIModelConfig cfg;
    cfg.id            = LLUUID::generateNewID().asString();
    cfg.type          = type;
    cfg.model         = getChild<LLUICtrl>("llm_model")->getValue().asString();
    cfg.endpoint      = getChild<LLUICtrl>("llm_endpoint")->getValue().asString();
    cfg.apikey        = getChild<LLUICtrl>("llm_apikey")->getValue().asString();
    cfg.context_length = (S32)getChild<LLUICtrl>("llm_context_length")->getValue().asReal();

    mConfigs.push_back(cfg);
    persistConfigs();
    renderList();
}

// ── Remove selected model configuration ──────────────────────────────────────

void FSFloaterAIModelConfig::onTrash()
{
    LLScrollListCtrl* list = getChild<LLScrollListCtrl>("model_config_list");
    LLScrollListItem* item = list->getFirstSelected();
    if (!item) return;

    std::string sel_id = item->getValue().asString();
    for (auto it = mConfigs.begin(); it != mConfigs.end(); ++it)
    {
        if (it->id == sel_id)
        {
            mConfigs.erase(it);
            persistConfigs();
            renderList();
            return;
        }
    }
}

// ── Apply: save edits + mark selected config as current ───────────────────────

void FSFloaterAIModelConfig::onApply()
{
    // First save current field edits (same as onSave)
    saveCurrentEdits();

    // Then mark selected config as current
    LLScrollListCtrl* list = getChild<LLScrollListCtrl>("model_config_list");
    LLScrollListItem* item = list->getFirstSelected();
    if (!item) return;

    std::string sel_id = item->getValue().asString();
    for (auto& cfg : mConfigs)
        cfg.is_current = (cfg.id == sel_id);

    persistConfigs();
    renderList();
}

// ── Shared helper: copy field values into the selected config ───────────────

void FSFloaterAIModelConfig::saveCurrentEdits()
{
    LLScrollListCtrl* list = getChild<LLScrollListCtrl>("model_config_list");
    LLScrollListItem* item = list->getFirstSelected();
    if (!item) return;

    std::string sel_id = item->getValue().asString();
    for (auto& cfg : mConfigs)
    {
        if (cfg.id == sel_id)
        {
            cfg.type          = getChild<LLUICtrl>("llm_type")->getValue().asString();
            cfg.model         = getChild<LLUICtrl>("llm_model")->getValue().asString();
            cfg.endpoint      = getChild<LLUICtrl>("llm_endpoint")->getValue().asString();
            cfg.apikey        = getChild<LLUICtrl>("llm_apikey")->getValue().asString();
            cfg.context_length = (S32)getChild<LLUICtrl>("llm_context_length")->getValue().asReal();
            break;
        }
    }
}

// ── Save: persist edits (without changing current marker) ────────────────────

void FSFloaterAIModelConfig::onSave()
{
    saveCurrentEdits();
    persistConfigs();
    renderList();
}

// ── Dynamic endpoint / API key behavior ───────────────────────────────────────

void FSFloaterAIModelConfig::onTypeChanged()
{
    std::string type = getChild<LLUICtrl>("llm_type")->getValue().asString();
    setEndpointAndApiKey(type);
}

void FSFloaterAIModelConfig::setEndpointAndApiKey(const std::string& type)
{
    LLLineEditor* endpoint = getChild<LLLineEditor>("llm_endpoint");
    LLLineEditor* apikey   = getChild<LLLineEditor>("llm_apikey");

    if (type == "ollama")
    {
        endpoint->setValue(LLSD("http://localhost:11434/api/chat"));
        apikey->setValue(LLSD(""));
        apikey->setEnabled(false);
    }
    else if (type == "openrouter")
    {
        endpoint->setValue(LLSD("https://openrouter.ai/api/v1/chat/completions"));
        apikey->setEnabled(true);
    }
    else if (type == "llamacpp")
    {
        endpoint->setValue(LLSD("http://localhost:8080/v1/chat/completions"));
        apikey->setValue(LLSD(""));
        apikey->setEnabled(false);
    }
    else if (type == "custom")
    {
        apikey->setEnabled(true);
    }
}

void FSFloaterAIModelConfig::onCloseClicked()
{
    closeFloater();
}

// ── Static persistence helpers ───────────────────────────────────────────────

void FSFloaterAIModelConfig::persistConfigs()
{
    LLSD list;
    for (const auto& cfg : mConfigs)
    {
        LLSD entry;
        entry["id"]            = cfg.id;
        entry["type"]          = cfg.type;
        entry["model"]         = cfg.model;
        entry["endpoint"]      = cfg.endpoint;
        entry["apikey"]        = cfg.apikey;
        entry["context_length"] = (int)cfg.context_length;
        entry["is_current"]     = (bool)cfg.is_current;
        list.append(entry);
    }
    gSavedSettings.setLLSD(kModelConfigSetting, list);
}

/*static*/
std::vector<AIModelConfig> FSFloaterAIModelConfig::loadConfigs()
{
    std::vector<AIModelConfig> result;
    LLSD list = gSavedSettings.getLLSD(kModelConfigSetting);
    for (LLSD::array_const_iterator it = list.beginArray(); it != list.endArray(); ++it)
    {
        AIModelConfig cfg;
        cfg.id            = (*it)["id"].asString();
        cfg.type          = (*it)["type"].asString();
        cfg.model         = (*it)["model"].asString();
        cfg.endpoint      = (*it)["endpoint"].asString();
        cfg.apikey        = (*it)["apikey"].asString();
        cfg.context_length = (*it)["context_length"].asInteger();
        if (cfg.context_length <= 0) cfg.context_length = 12000;
        cfg.is_current     = (*it)["is_current"].asBoolean();
        result.push_back(cfg);
    }
    return result;
}

// ── FSFloaterDiscordContacts ──────────────────────────────────────────────────

static const int    DISCORD_CONTACTS_PORT = 3002;
static const char*  DISCORD_CONTACTS_HOST = "127.0.0.1";

FSFloaterDiscordContacts::FSFloaterDiscordContacts(const LLSD& key)
    : LLFloater(key)
{
}

bool FSFloaterDiscordContacts::postBuild()
{
    getChild<LLButton>("dc_open_btn")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { openChatForSelected(); });
    getChild<LLButton>("dc_refresh_btn")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { fetchContacts(); });
    getChild<LLButton>("dc_close_btn")->setCommitCallback(
        [this](LLUICtrl*, const LLSD&) { closeFloater(); });

    // Double-click on a row opens chat
    getChild<LLScrollListCtrl>("contacts_list")->setDoubleClickCallback(
        [this]() { openChatForSelected(); });

    return LLFloater::postBuild();
}

void FSFloaterDiscordContacts::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    fetchContacts();
}

void FSFloaterDiscordContacts::draw()
{
    // Pick up result queued by the background fetch thread
    if (mPendingReady)
    {
        std::vector<DiscordContact> contacts;
        {
            std::lock_guard<std::mutex> lk(mPendingMutex);
            contacts.swap(mPending);
            mPendingReady = false;
            mFetching     = false;
        }
        populateList(contacts);
    }
    LLFloater::draw();
}

void FSFloaterDiscordContacts::fetchContacts()
{
    if (mFetching) return;
    mFetching = true;

    getChild<LLScrollListCtrl>("contacts_list")->clearRows();

    std::thread([this]()
    {
        std::vector<DiscordContact> result;

        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) { goto done; }

        {
            struct sockaddr_in addr;
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(DISCORD_CONTACTS_PORT);
            ::inet_pton(AF_INET, DISCORD_CONTACTS_HOST, &addr.sin_addr);

            if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
            {
                ::close(sock);
                goto done;
            }

            const std::string req =
                "GET /contacts HTTP/1.1\r\n"
                "Host: 127.0.0.1:3002\r\n"
                "Connection: close\r\n"
                "\r\n";
            ::send(sock, req.c_str(), req.size(), 0);

            // Read full response
            std::string response;
            char buf[4096];
            ssize_t n;
            while ((n = ::recv(sock, buf, sizeof(buf), 0)) > 0)
                response.append(buf, n);
            ::close(sock);

            // Find body after double CRLF
            std::size_t body_pos = response.find("\r\n\r\n");
            if (body_pos == std::string::npos) goto done;
            std::string body = response.substr(body_pos + 4);

            // Skip chunked size line if present
            if (!body.empty() && std::isxdigit((unsigned char)body[0]))
            {
                std::size_t lf = body.find('\n');
                if (lf != std::string::npos) body = body.substr(lf + 1);
            }

            try
            {
                boost::json::value jv = boost::json::parse(body);
                if (!jv.is_array()) goto done;
                for (const auto& jc : jv.as_array())
                {
                    if (!jc.is_object()) continue;
                    const auto& obj = jc.as_object();
                    DiscordContact c;
                    c.discord_id   = obj.contains("discord_id")   ? std::string(obj.at("discord_id").as_string())   : "";
                    c.display_name = obj.contains("display_name")  ? std::string(obj.at("display_name").as_string()) : "";
                    c.status       = obj.contains("status")        ? std::string(obj.at("status").as_string())       : "offline";
                    c.server       = obj.contains("server")        ? std::string(obj.at("server").as_string())       : "";
                    if (c.discord_id.empty()) continue;
                    c.uuid = discordUUID(c.discord_id);
                    result.push_back(c);
                }
            }
            catch (...) {}
        }

    done:
        std::lock_guard<std::mutex> lk(mPendingMutex);
        mPending      = std::move(result);
        mPendingReady = true;

    }).detach();
}

void FSFloaterDiscordContacts::populateList(const std::vector<DiscordContact>& contacts)
{
    mContacts = contacts;

    // Register each contact in the avatar name cache so Firestorm treats them
    // as valid "avatars" and doesn't try to resolve them against SL servers.
    for (const auto& c : mContacts)
    {
        LLAvatarName av;
        // For channels, include the server/guild name in the display name
        bool is_chan = (c.status == "channel" || c.status == "channel_muted");
        std::string srv_name = c.server.empty() ? "UNKNOWN_SERVER" : c.server;
        std::string full = (is_chan)
            ? c.display_name + " / " + srv_name + " (discord)"
            : c.display_name + " (discord)";
        av.fromLLSD(LLSD()
            .with("display_name",             full)
            .with("username",                 full)
            .with("legacy_first_name",        std::string(""))
            .with("legacy_last_name",         std::string(""))
            .with("is_display_name_default",  true)
            .with("display_name_expires",     std::string("2099-01-01T00:00:00Z"))
            .with("display_name_next_update", std::string("2099-01-01T00:00:00Z")));
        LLAvatarNameCache::instance().insert(c.uuid, av);
    }

    LLScrollListCtrl* list = getChild<LLScrollListCtrl>("contacts_list");
    list->clearRows();

    static const std::string kDot = "\xe2\x97\x8f"; // ● U+25CF
    for (const auto& c : mContacts)
    {
        std::string glyph;
        LLColor4    dot_color;
        bool        has_color = true;
        if (c.status == "online")
        {
            glyph     = kDot;
            dot_color = LLColor4(0.13f, 0.80f, 0.13f, 1.f);
        }
        else if (c.status == "idle")
        {
            glyph     = kDot;
            dot_color = LLColor4(1.00f, 0.75f, 0.00f, 1.f);
        }
        else if (c.status == "dnd")
        {
            glyph     = kDot;
            dot_color = LLColor4(0.85f, 0.10f, 0.10f, 1.f);
        }
        else if (c.status == "channel")
        {
            glyph     = kDot;
            dot_color = LLColor4(0.40f, 0.60f, 1.00f, 1.f);
        }
        else if (c.status == "channel_muted")
        {
            glyph     = kDot;
            dot_color = LLColor4(0.45f, 0.45f, 0.45f, 1.f);
        }
        else
        {
            has_color = false;
        }

        LLSD row;
        row["id"] = c.uuid;
        row["columns"][0]["column"] = "col_status";
        row["columns"][0]["value"]  = glyph;
        row["columns"][1]["column"] = "col_name";
        row["columns"][1]["value"]  = c.display_name;
        row["columns"][2]["column"] = "col_server";
        row["columns"][2]["value"]  = c.server;
        LLScrollListItem* item = list->addElement(row);
        if (item && has_color)
        {
            LLScrollListCell* cell = item->getColumn(0);
            if (cell) cell->setColor(dot_color);
        }
    }
}

void FSFloaterDiscordContacts::openChatForSelected()
{
    LLScrollListCtrl* list = getChild<LLScrollListCtrl>("contacts_list");
    LLScrollListItem* item = list->getFirstSelected();
    if (!item) return;

    LLUUID uuid = item->getUUID();

    // Find matching contact
    const DiscordContact* found = nullptr;
    for (const auto& c : mContacts)
        if (c.uuid == uuid) { found = &c; break; }
    if (!found) return;

    // Build the display name including server name for channels
    bool is_chan = (found->status == "channel" || found->status == "channel_muted");
    std::string srv_name = found->server.empty() ? "UNKNOWN_SERVER" : found->server;
    std::string full_name = (is_chan)
        ? found->display_name + " / " + srv_name + " (discord)"
        : found->display_name + " (discord)";
    LL_INFOS("Discord") << "[DISCORD-TITLE-DBG] openChatForSelected: display_name='" << found->display_name
                        << "' server='" << found->server << "' full_name='" << full_name << "'" << LL_ENDL;

    // Pre-compute real_session_id (P2P: XOR of other_id and agent_id) and
    // register BEFORE addSession so isDiscordSession() is already true when
    // onAvatarNameCache fires synchronously inside addSession.
    LLUUID real_session_id = found->uuid ^ gAgent.getID();
    {
        std::lock_guard<std::mutex> lk(sDiscordMutex);
        sDiscordSessions[real_session_id]      = full_name;
        sDiscordOriginalUUIDs[real_session_id] = found->uuid;
        if (is_chan)
        {
            sDiscordChannelIds[real_session_id] = found->discord_id;
        }
    }

    // Mirror the updated avatar name under the pre-computed session UUID
    LLAvatarName av;
    if (LLAvatarNameCache::get(found->uuid, &av))
        LLAvatarNameCache::instance().insert(real_session_id, av);

    // Open (or focus) the IM session
    LLUUID session_id = LLIMMgr::instance().addSession(
        full_name, IM_NOTHING_SPECIAL, found->uuid);
    if (session_id.isNull())
    {
        std::lock_guard<std::mutex> lk(sDiscordMutex);
        sDiscordSessions.erase(real_session_id);
        sDiscordOriginalUUIDs.erase(real_session_id);
        sDiscordChannelIds.erase(real_session_id);
        return;
    }
    // Correct if viewer computed a different id than predicted
    if (session_id != real_session_id)
    {
        std::lock_guard<std::mutex> lk(sDiscordMutex);
        sDiscordSessions.erase(real_session_id);
        sDiscordOriginalUUIDs.erase(real_session_id);
        sDiscordChannelIds.erase(real_session_id);
        sDiscordSessions[session_id]      = full_name;
        sDiscordOriginalUUIDs[session_id] = found->uuid;
        if (is_chan) sDiscordChannelIds[session_id] = found->discord_id;
        if (LLAvatarNameCache::get(found->uuid, &av))
            LLAvatarNameCache::instance().insert(session_id, av);
        real_session_id = session_id;
    }

    // Force title immediately — handles the case where postBuild already fired
    // with a stale mName (e.g. session restored from a previous run).
    FSFloaterIM* floater = FSFloaterIM::findInstance(real_session_id);
    if (floater)
        floater->updateSessionName(full_name, full_name);

    // Enable the channel on the relay (channel sessions only)
    if (is_chan)
    {
        postToRelay("/channel_active",
            "{\"channel_id\":" + found->discord_id + ",\"active\":true}");
    }
}

// ── FSFloaterAIConfig ─────────────────────────────────────────────────────────

/*static*/ std::map<LLUUID, AIConfigState> FSFloaterAIConfig::sConfigs;
/*static*/ LLUUID                          FSFloaterAIConfig::sCurrentEditingSession;
/*static*/ LLUUID                          FSFloaterAIConfig::sPendingLoadSessionID;
/*static*/ LLUUID                          FSFloaterAIConfig::sPendingSessionSessionID;
/*static*/ std::string                     FSFloaterAIConfig::sPendingSessionJSON;

FSFloaterAIConfig::FSFloaterAIConfig(const LLSD& key)
    : LLFloater(key)
{
}

void FSFloaterAIConfig::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);
    // Use the session that was active when the menu was clicked (set in draw())
    mCurrentSessionID = sCurrentEditingSession;

    // Reflect current session in the agent selector combo
    getChild<LLComboBox>("ai_agent_select")->setCurrentByID(mCurrentSessionID);

    const AIConfigState& cfg = sConfigs[mCurrentSessionID];
    getChild<LLLineEditor>("ai_config_name")->setText(cfg.name);
    getChild<LLTextEditor>("ai_config_persona")->setText(cfg.persona);
    getChild<LLTextEditor>("ai_config_instructions")->setText(cfg.instructions);
    getChild<LLCheckBoxCtrl>("ai_config_web_search")->setValue(LLSD(cfg.web_search));
    getChild<LLTextBox>("ai_config_name_error")->setVisible(false);
    // Ask the server for the live config — routes to applySnapshot() asynchronously
    sendMCPRequest(mCurrentSessionID, "/config get");
}

bool FSFloaterAIConfig::postBuild()
{
    getChild<LLButton>("ai_ok_btn")->setCommitCallback(
        boost::bind(&FSFloaterAIConfig::onOKClicked, this));
    getChild<LLButton>("ai_quit_btn")->setCommitCallback(
        boost::bind(&FSFloaterAIConfig::onQuitClicked, this));
    getChild<LLButton>("ai_save_btn")->setCommitCallback(
        boost::bind(&FSFloaterAIConfig::onSaveClicked, this));
    getChild<LLButton>("ai_load_btn")->setCommitCallback(
        boost::bind(&FSFloaterAIConfig::onLoadClicked, this));

    // Populate agent selector combo
    LLComboBox* combo = getChild<LLComboBox>("ai_agent_select");
    {
        std::string name1 = "AI Agent";
        if (gSavedSettings.controlExists("FSAIAgentName1"))
            name1 = gSavedSettings.getString("FSAIAgentName1");
        std::string name2 = "AI Agent 2";
        if (gSavedSettings.controlExists("FSAIAgentName2"))
            name2 = gSavedSettings.getString("FSAIAgentName2");
        combo->clearRows();
        combo->add(name1, LLSD(AI_AGENT_SESSION_ID));
        combo->add(name2, LLSD(AI_AGENT_2_SESSION_ID));
    }
    combo->setCommitCallback(boost::bind(&FSFloaterAIConfig::onAgentChanged, this));

    return LLFloater::postBuild();
}

void FSFloaterAIConfig::onOKClicked()
{
    std::string name = getChild<LLLineEditor>("ai_config_name")->getText();
    LLStringUtil::trim(name);

    if (name.empty())
    {
        getChild<LLTextBox>("ai_config_name_error")->setVisible(true);
        return;
    }

    getChild<LLTextBox>("ai_config_name_error")->setVisible(false);
    AIConfigState& cfg   = sConfigs[mCurrentSessionID];
    cfg.name         = name;
    cfg.persona      = getChild<LLTextEditor>("ai_config_persona")->getText();
    cfg.instructions = getChild<LLTextEditor>("ai_config_instructions")->getText();
    cfg.web_search   = getChild<LLCheckBoxCtrl>("ai_config_web_search")->getValue().asBoolean();

    boost::json::object payload;
    payload["name"]         = cfg.name;
    payload["persona"]      = cfg.persona;
    payload["instructions"] = cfg.instructions;
    payload["web_tools"]    = cfg.web_search;
    sendMCPRequest(mCurrentSessionID, "/config apply " + boost::json::serialize(payload));

    closeFloater();
}

void FSFloaterAIConfig::onQuitClicked()
{
    closeFloater();
}

void FSFloaterAIConfig::onAgentChanged()
{
    LLComboBox* combo = getChild<LLComboBox>("ai_agent_select");
    LLUUID new_session = combo->getCurrentID();
    if (new_session == mCurrentSessionID) return;

    mCurrentSessionID = new_session;
    sCurrentEditingSession = new_session;

    const AIConfigState& cfg = sConfigs[mCurrentSessionID];
    getChild<LLLineEditor>("ai_config_name")->setText(cfg.name);
    getChild<LLTextEditor>("ai_config_persona")->setText(cfg.persona);
    getChild<LLTextEditor>("ai_config_instructions")->setText(cfg.instructions);
    getChild<LLCheckBoxCtrl>("ai_config_web_search")->setValue(LLSD(cfg.web_search));
    getChild<LLTextBox>("ai_config_name_error")->setVisible(false);
    // Refresh from the live server for the newly selected agent
    sendMCPRequest(mCurrentSessionID, "/config get");
}

void FSFloaterAIConfig::onSaveClicked()
{
    std::string name = getChild<LLLineEditor>("ai_config_name")->getText();
    LLStringUtil::trim(name);
    if (name.empty())
    {
        LLSD args;
        args["MESSAGE"] = "Cannot save: a config name is required.\nEnter a name in the Config name field or use /config name <name>.";
        LLNotificationsUtil::add("GenericAlert", args);
        return;
    }
    sConfigs[mCurrentSessionID].name = name;
    LLFilePickerReplyThread::startPicker(
        boost::bind(&FSFloaterAIConfig::onSaveFileSelected, this, _1),
        LLFilePicker::FFSAVE_ALL,
        name + ".json");
}

void FSFloaterAIConfig::onSaveFileSelected(const std::vector<std::string>& filenames)
{
    if (filenames.empty()) return;

    std::string selected = filenames[0];
    std::string output_path = selected;

    // Extract file name (without extension) to sync cfg.name
    size_t last_slash = selected.find_last_of("/\\");
    size_t last_dot = selected.find_last_of(".");
    std::string name_without_ext;
    if (last_slash != std::string::npos && last_dot != std::string::npos && last_dot > last_slash)
    {
        name_without_ext = selected.substr(last_slash + 1, last_dot - last_slash - 1);
    }
    else if (last_slash != std::string::npos)
    {
        name_without_ext = selected.substr(last_slash + 1);
    }
    else
    {
        name_without_ext = selected;
    }

    // Update sConfigs and the UI with the file name
    sConfigs[mCurrentSessionID].name = name_without_ext;
    getChild<LLLineEditor>("ai_config_name")->setText(name_without_ext);

    boost::json::object obj;
    obj["name"]         = name_without_ext;
    obj["persona"]      = getChild<LLTextEditor>("ai_config_persona")->getText();
    obj["instructions"] = getChild<LLTextEditor>("ai_config_instructions")->getText();
    obj["web_tools"]    = getChild<LLCheckBoxCtrl>("ai_config_web_search")->getValue().asBoolean();

    std::ofstream file(output_path);
    if (file.is_open())
    {
        file << boost::json::serialize(obj);
        LL_INFOS() << "AI config saved to " << output_path << LL_ENDL;
    }
    else
    {
        LL_WARNS() << "Failed to open file for writing: " << output_path << LL_ENDL;
    }
}

void FSFloaterAIConfig::onLoadClicked()
{
    LLFilePickerReplyThread::startPicker(
        boost::bind(&FSFloaterAIConfig::onLoadFileSelected, this, _1),
        LLFilePicker::FFLOAD_ALL,
        false);
}

void FSFloaterAIConfig::onLoadFileSelected(const std::vector<std::string>& filenames)
{
    if (filenames.empty()) return;

    std::ifstream file(filenames[0]);
    if (!file.is_open()) { LL_WARNS() << "Cannot open: " << filenames[0] << LL_ENDL; return; }

    std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    boost::system::error_code ec;
    boost::json::value val = boost::json::parse(contents, ec);
    if (ec) { LL_WARNS() << "JSON parse error: " << ec.message() << LL_ENDL; return; }

    AIConfigState& cfg = sConfigs[mCurrentSessionID];
    auto& obj = val.as_object();
    if (obj.contains("name")         && obj["name"].is_string())
        cfg.name         = std::string(obj["name"].as_string());
    if (obj.contains("persona")      && obj["persona"].is_string())
        cfg.persona      = std::string(obj["persona"].as_string());
    if (obj.contains("instructions") && obj["instructions"].is_string())
        cfg.instructions = std::string(obj["instructions"].as_string());
    if (obj.contains("web_tools")    && obj["web_tools"].is_bool())
        cfg.web_search   = obj["web_tools"].as_bool();

    getChild<LLLineEditor>("ai_config_name")->setText(cfg.name);
    getChild<LLTextEditor>("ai_config_persona")->setText(cfg.persona);
    getChild<LLTextEditor>("ai_config_instructions")->setText(cfg.instructions);
    getChild<LLCheckBoxCtrl>("ai_config_web_search")->setValue(LLSD(cfg.web_search));
    getChild<LLTextBox>("ai_config_name_error")->setVisible(false);
    LL_INFOS() << "AI config loaded from " << filenames[0] << LL_ENDL;
}

/*static*/
void FSFloaterAIConfig::openLoadPickerForSession(const LLUUID& session_id)
{
    sPendingLoadSessionID = session_id;
    LLFilePickerReplyThread::startPicker(
        boost::bind(&FSFloaterAIConfig::onLoadForSessionFileSelected, _1),
        LLFilePicker::FFLOAD_ALL,
        false);
}

/*static*/
void FSFloaterAIConfig::onLoadForSessionFileSelected(const std::vector<std::string>& filenames)
{
    if (filenames.empty()) return;

    std::ifstream file(filenames[0]);
    if (!file.is_open()) { LL_WARNS() << "Cannot open: " << filenames[0] << LL_ENDL; return; }

    std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    boost::system::error_code ec;
    boost::json::value val = boost::json::parse(contents, ec);
    if (ec) { LL_WARNS() << "JSON parse error: " << ec.message() << LL_ENDL; return; }

    AIConfigState& cfg = sConfigs[sPendingLoadSessionID];
    auto& obj = val.as_object();
    if (obj.contains("name")         && obj["name"].is_string())
        cfg.name         = std::string(obj["name"].as_string());
    if (obj.contains("persona")      && obj["persona"].is_string())
        cfg.persona      = std::string(obj["persona"].as_string());
    if (obj.contains("instructions") && obj["instructions"].is_string())
        cfg.instructions = std::string(obj["instructions"].as_string());
    if (obj.contains("web_tools")    && obj["web_tools"].is_bool())
        cfg.web_search   = obj["web_tools"].as_bool();

    // Refresh the floater if it's open for this session
    FSFloaterAIConfig* instance = LLFloaterReg::findTypedInstance<FSFloaterAIConfig>("fs_ai_config");
    if (instance && instance->isInVisibleChain() && instance->mCurrentSessionID == sPendingLoadSessionID)
    {
        instance->getChild<LLLineEditor>("ai_config_name")->setText(cfg.name);
        instance->getChild<LLTextEditor>("ai_config_persona")->setText(cfg.persona);
        instance->getChild<LLTextEditor>("ai_config_instructions")->setText(cfg.instructions);
        instance->getChild<LLCheckBoxCtrl>("ai_config_web_search")->setValue(LLSD(cfg.web_search));
    }

    boost::json::object payload;
    payload["name"]         = cfg.name;
    payload["persona"]      = cfg.persona;
    payload["instructions"] = cfg.instructions;
    payload["web_tools"]    = cfg.web_search;
    sendMCPRequest(sPendingLoadSessionID, "/config apply " + boost::json::serialize(payload));

    LL_INFOS() << "AI config loaded from " << filenames[0] << LL_ENDL;
}

/*static*/
void FSFloaterAIConfig::onServerReset(const LLUUID& session_id)
{
    sConfigs[session_id] = AIConfigState{};   // reset to defaults

    FSFloaterAIConfig* instance = LLFloaterReg::findTypedInstance<FSFloaterAIConfig>("fs_ai_config");
    if (instance && instance->isInVisibleChain() && instance->mCurrentSessionID == session_id)
    {
        instance->getChild<LLLineEditor>("ai_config_name")->setText(std::string());
        instance->getChild<LLTextEditor>("ai_config_persona")->setText(std::string());
        instance->getChild<LLTextEditor>("ai_config_instructions")->setText(std::string());
        instance->getChild<LLCheckBoxCtrl>("ai_config_web_search")->setValue(LLSD(true));
        instance->getChild<LLTextBox>("ai_config_name_error")->setVisible(false);
    }
}

/*static*/
void FSFloaterAIConfig::applySnapshot(const LLUUID& session_id,
                                       const std::string& name,
                                       const std::string& persona,
                                       const std::string& instructions,
                                       bool web_tools)
{
    AIConfigState& cfg   = sConfigs[session_id];
    cfg.name         = name;
    cfg.persona      = persona;
    cfg.instructions = instructions;
    cfg.web_search   = web_tools;

    FSFloaterAIConfig* instance = LLFloaterReg::findTypedInstance<FSFloaterAIConfig>("fs_ai_config");
    if (instance && instance->isInVisibleChain() && instance->mCurrentSessionID == session_id)
    {
        instance->getChild<LLLineEditor>("ai_config_name")->setText(cfg.name);
        instance->getChild<LLTextEditor>("ai_config_persona")->setText(cfg.persona);
        instance->getChild<LLTextEditor>("ai_config_instructions")->setText(cfg.instructions);
        instance->getChild<LLCheckBoxCtrl>("ai_config_web_search")->setValue(LLSD(cfg.web_search));
    }
}

/*static*/
void FSFloaterAIConfig::applySnapshot(const std::string& name, const std::string& persona, const std::string& instructions, bool web_tools)
{
    // Overload without session_id — uses sCurrentEditingSession (called during /config get response)
    applySnapshot(sCurrentEditingSession, name, persona, instructions, web_tools);
}

// ── Session save (export) ─────────────────────────────────────────────────────

/*static*/
void FSFloaterAIConfig::onSessionExport(const LLUUID& session_id, const std::string& json)
{
    // Called when the server returns a session_export blob.
    // Store it and open the file-save picker immediately.
    sPendingSessionSessionID = session_id;
    sPendingSessionJSON      = json;

    // Build a suggested filename from the config name if available
    std::string suggested = "session";
    auto it = sConfigs.find(session_id);
    if (it != sConfigs.end() && !it->second.name.empty())
        suggested = it->second.name;

    LLFilePickerReplyThread::startPicker(
        boost::bind(&FSFloaterAIConfig::onSessionSaveFileSelected, _1),
        LLFilePicker::FFSAVE_ALL,
        suggested + ".json");
}

/*static*/
void FSFloaterAIConfig::onSessionSaveFileSelected(const std::vector<std::string>& filenames)
{
    if (filenames.empty() || sPendingSessionJSON.empty()) return;

    // The picker may return a path without extension; ensure .json suffix
    std::string path = filenames[0];
    if (path.size() < 5 || path.substr(path.size() - 5) != ".json")
        path += ".json";

    std::ofstream out(path);
    if (out.is_open())
        out << sPendingSessionJSON;

    sPendingSessionJSON.clear();
}

// ── Session load ──────────────────────────────────────────────────────────────

/*static*/
void FSFloaterAIConfig::openSessionLoadPicker(const LLUUID& session_id)
{
    sPendingSessionSessionID = session_id;
    LLFilePickerReplyThread::startPicker(
        boost::bind(&FSFloaterAIConfig::onSessionLoadFileSelected, _1),
        LLFilePicker::FFLOAD_ALL,
        false);
}

/*static*/
void FSFloaterAIConfig::onSessionLoadFileSelected(const std::vector<std::string>& filenames)
{
    if (filenames.empty()) return;

    std::ifstream in(filenames[0]);
    if (!in.is_open()) return;
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    // Parse and validate
    boost::json::value jv;
    try { jv = boost::json::parse(raw); } catch (...) { return; }
    if (!jv.is_object()) return;
    auto& obj = jv.as_object();

    const LLUUID session_id = sPendingSessionSessionID;

    // 1. Apply config fields locally
    std::string name         = obj.contains("name")         && obj["name"].is_string()         ? std::string(obj["name"].as_string())         : "";
    std::string persona      = obj.contains("persona")      && obj["persona"].is_string()      ? std::string(obj["persona"].as_string())      : "";
    std::string instructions = obj.contains("instructions") && obj["instructions"].is_string() ? std::string(obj["instructions"].as_string()) : "";
    bool        web_tools    = obj.contains("web_tools")    && obj["web_tools"].is_bool()      ? obj["web_tools"].as_bool()                   : true;
    applySnapshot(session_id, name, persona, instructions, web_tools);

    // 2. Rebuild the visible chat history
    LLIMModel::LLIMSession* imsession = LLIMModel::instance().findIMSession(session_id);
    if (imsession && obj.contains("history") && obj["history"].is_array())
    {
        imsession->mMsgs.clear();

        // Determine display names
        std::string agent_name = (session_id == AI_AGENT_2_SESSION_ID) ? "AI Agent 2" : "AI Agent";
        if (session_id == AI_AGENT_SESSION_ID && gSavedSettings.controlExists("FSAIAgentName1"))
            agent_name = gSavedSettings.getString("FSAIAgentName1");
        else if (session_id == AI_AGENT_2_SESSION_ID && gSavedSettings.controlExists("FSAIAgentName2"))
            agent_name = gSavedSettings.getString("FSAIAgentName2");

        std::string user_name = "You";
        LLAgentUI::buildFullname(user_name);

        // History is oldest-first; addMessage pushes to front, so iterate in reverse
        const auto& hist = obj.at("history").as_array();
        for (auto it = hist.rbegin(); it != hist.rend(); ++it)
        {
            if (!it->is_object()) continue;
            const auto& m = it->as_object();
            std::string role    = m.contains("role")    && m.at("role").is_string()    ? std::string(m.at("role").as_string())    : "";
            std::string content = m.contains("content") && m.at("content").is_string() ? std::string(m.at("content").as_string()) : "";
            if (content.empty()) continue;

            std::string from = (role == "assistant") ? agent_name : user_name;
            LLUUID       fid = (role == "assistant") ? session_id : LLUUID::null;
            imsession->addMessage(from, fid, content, "", CHAT_STYLE_HISTORY, false, 0);
        }

        // Refresh the IM floater display
        FSFloaterIM* floater = LLFloaterReg::findTypedInstance<FSFloaterIM>("panel_ai_agent", session_id);
        if (floater)
            floater->reloadMessages(false);
    }

    // 3. Send the full session to the server to restore its state
    sendMCPRequest(session_id, "/session import " + raw);
}

// EOF
