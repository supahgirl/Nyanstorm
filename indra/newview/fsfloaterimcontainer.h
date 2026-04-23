/**
 * @file fsfloaterimcontainer.h
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

// Original file: llimfloatercontainer.h


#ifndef FS_FLOATERIMCONTAINER_H
#define FS_FLOATERIMCONTAINER_H

#include "llmultifloater.h"
#include "llimview.h"
#include "llinstantmessage.h"

class LLTabContainer;

class FSFloaterIMContainer : public LLMultiFloater, public LLIMSessionObserver
{
public:
    FSFloaterIMContainer(const LLSD& seed);
    virtual ~FSFloaterIMContainer();

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onClose(bool app_quitting) override;
    void onCloseFloater(LLUUID& id);
    void draw() override;
    void addFloater(LLFloater* floaterp, 
                    bool select_added_floater,
                    LLTabContainer::eInsertionPoint insertion_point = LLTabContainer::END) override;

    void addFloater(LLFloater* floaterp,
                    bool select_added_floater,
                    EInstantMessage type,
                    LLTabContainer::eInsertionPoint no_auto_insertion_point = LLTabContainer::END);

// [SL:KB] - Patch: Chat-NearbyChatBar | Checked: 2011-12-11 (Catznip-3.2.0d) | Added: Catznip-3.2.0d
    void removeFloater(LLFloater* floaterp) override;
// [/SL:KB]
    bool hasFloater(LLFloater* floaterp);

    void addNewSession(LLFloater* floaterp, EInstantMessage type);

    static FSFloaterIMContainer* findInstance();
    static FSFloaterIMContainer* getInstance();

    F32 getCurrentTransparency() override;

    void setVisible(bool b) override;
    void setMinimized(bool b) override;

    void onNewMessageReceived(const LLSD& msg);

    void sessionAdded(const LLUUID& session_id, const std::string& name, const LLUUID& other_participant_id, bool has_offline_msg) override;
    void sessionActivated(const LLUUID& session_id, const std::string& name, const LLUUID& other_participant_id) override {};
    void sessionVoiceOrIMStarted(const LLUUID& session_id) override {};
    void sessionRemoved(const LLUUID& session_id) override;
    void sessionIDUpdated(const LLUUID& old_session_id, const LLUUID& new_session_id) override;

    static void reloadEmptyFloaters();
    void initTabs();

    void addFlashingSession(const LLUUID& session_id);

    void tabOpen(LLFloater* opened_floater, bool from_click) override;

    void startFlashingTab(LLFloater* floater, const std::string& message);

    // <FS:PP> Restore open IMs from previous session
    void saveOpenIMs();
    void restoreOpenIMs();
    // </FS:PP>

private:
    enum eVoiceState
    {
        VOICE_STATE_NONE,
        VOICE_STATE_UNKNOWN,
        VOICE_STATE_CONNECTED,
        VOICE_STATE_NOT_CONNECTED,
        VOICE_STATE_ERROR
    };

    LLFloater*  getCurrentVoiceFloater();
    void        onVoiceStateIndicatorChanged(const LLSD& data);

    LLFloater*  mActiveVoiceFloater;
    LLTimer     mActiveVoiceUpdateTimer;
    eVoiceState mCurrentVoiceState;
    bool        mForceVoiceStateUpdate;

    typedef std::map<LLUUID, LLFloater*> avatarID_panel_map_t;
    avatarID_panel_map_t mSessions;
    boost::signals2::connection mNewMessageConnection;

    void checkFlashing();
    uuid_vec_t  mFlashingSessions;

    bool        mIsAddingNewSession;

    std::map<LLFloater*, bool> mFlashingTabStates;

    void onTabSelectedDiscord();

// [SL:KB] - Patch: UI-TabRearrange | Checked: 2012-05-05 (Catznip-3.3.0)
protected:
    void onIMTabRearrange(S32 tab_index, LLPanel* tab_panel);
// [/SL:KB]

};

#include "fsfloaterim.h"
#include <map>
#include <mutex>
#include <string>

extern const LLUUID AI_AGENT_SESSION_ID;
extern const LLUUID AI_AGENT_2_SESSION_ID;

// ── Discord session registry ──────────────────────────────────────────────────
extern std::map<LLUUID, std::string> sDiscordSessions;      // real_session_id → display_name
extern std::map<LLUUID, LLUUID>      sDiscordOriginalUUIDs;  // real_session_id → original discord UUID
extern std::map<LLUUID, std::string> sDiscordChannelIds;     // session_id → discord channel_id (str)
extern std::mutex                    sDiscordMutex;

LLUUID      discordUUID(const std::string& discord_id);
void        postToRelay(const std::string& path, const std::string& body);
bool        isDiscordSession(const LLUUID& session_id);
std::string getDiscordDisplayName(const LLUUID& session_id);
void        discordUpdateStatusFromRelay(const std::string& status);

// ── Discord Contacts floater ──────────────────────────────────────────────────

struct DiscordContact
{
    std::string discord_id;
    std::string display_name;   // raw name (without " (discord)" suffix)
    std::string status;         // "online"|"idle"|"dnd"|"offline"|"channel"|"channel_muted"
    std::string server;         // guild name for channels, empty for friends
    LLUUID      uuid;           // discordUUID(discord_id)
};

class FSFloaterDiscordContacts : public LLFloater
{
public:
    FSFloaterDiscordContacts(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    void fetchContacts();
    void populateList(const std::vector<DiscordContact>& contacts);
    void openChatForSelected();

    std::vector<DiscordContact> mContacts;
    std::vector<DiscordContact> mPending;
    std::mutex                  mPendingMutex;
    bool                        mPendingReady = false;
    bool                        mFetching     = false;
};

// ── AI Config floater (forward declaration for FSFloaterAIAgent::draw) ────────
class FSFloaterAIConfig;

class FSFloaterAIAgent : public FSFloaterIM
{
public:
	FSFloaterAIAgent(const LLUUID& session_id);
    bool postBuild() override;
    void draw() override;
    static FSFloaterAIAgent* getInstance(const LLUUID& session_id = AI_AGENT_SESSION_ID);
};

// ── AI History notecard ───────────────────────────────────────────────────────
// Called from onMCPServerSuccess when type=="history_export"
void aiCreateHistoryNotecard(const LLUUID& session_id,
                             const std::vector<std::pair<std::string,std::string>>& history);

// ── AI Model List floater ─────────────────────────────────────────────────────

class FSFloaterAIModelList : public LLFloater
{
public:
    FSFloaterAIModelList(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    // Called from onMCPServerSuccess when type=="model_list"
    static void updateModels(const std::vector<std::tuple<std::string, std::string, std::string, bool>>& models);

private:
    LLUUID mSessionID;
    std::vector<std::tuple<std::string, std::string, std::string, bool>> mModels; // manager, name, endpoint, active

    void renderModelList();
    void notifyAgents(const std::string& type, const std::string& name);
    void onOKClicked();
    void onApplyClicked();
    void onQuitClicked();
};

// ── AI Config floater ──────────────────────────────────────────────────────────

struct AIConfigState
{
    std::string name;
    std::string persona;
    std::string instructions;
    bool        web_search = true;
};

class FSFloaterAIConfig : public LLFloater
{
public:
    FSFloaterAIConfig(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    // Per-session config state — keyed by AI agent session UUID
    static std::map<LLUUID, AIConfigState> sConfigs;
    // Set by FSFloaterAIAgent::draw() so onOpen knows which session to edit
    static LLUUID sCurrentEditingSession;

    static void onServerReset(const LLUUID& session_id);
    static void applySnapshot(const LLUUID& session_id,
                              const std::string& name,
                              const std::string& persona,
                              const std::string& instructions,
                              bool web_tools);
    // Overload used when session_id is unknown — falls back to sCurrentEditingSession
    static void applySnapshot(const std::string& name,
                              const std::string& persona,
                              const std::string& instructions,
                              bool web_tools);
    static void openLoadPickerForSession(const LLUUID& session_id);

    // ── Session save/load ─────────────────────────────────────────────────
    // Called when /session export response arrives; immediately opens save picker
    static void onSessionExport(const LLUUID& session_id, const std::string& json);
    // Called when /session load slash command is typed
    static void openSessionLoadPicker(const LLUUID& session_id);

private:
    LLUUID mCurrentSessionID; // session this floater instance is currently editing

    void onOKClicked();
    void onQuitClicked();
    void onSaveClicked();
    void onLoadClicked();
    void onAgentChanged();
    void onSaveFileSelected(const std::vector<std::string>& filenames);
    void onLoadFileSelected(const std::vector<std::string>& filenames);
    static void onLoadForSessionFileSelected(const std::vector<std::string>& filenames);
    static LLUUID sPendingLoadSessionID;

    static void onSessionSaveFileSelected(const std::vector<std::string>& filenames);
    static void onSessionLoadFileSelected(const std::vector<std::string>& filenames);
    static LLUUID   sPendingSessionSessionID;
    static std::string sPendingSessionJSON;
};

#endif // FS_FLOATERIMCONTAINER_H
