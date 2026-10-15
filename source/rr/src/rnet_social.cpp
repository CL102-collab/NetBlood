// SPDX-License-Identifier: GPL-2.0-or-later
#include "rnet_social.h"
#include "rnet_social_model.h"
#include "rnet.h"
#include "duke3d.h"
#include "input.h"
#include "rnet_rules.h"
#include "osd.h"
#include "timer.h"
#include "../../blood/src/netcountry.h"
#include "../../blood/src/netlag.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>

extern char smalltextfont[2048];

namespace rnet { namespace social {
namespace {
enum : uint8_t { RosterMessage = SocialFirst, ChatRequest, ChatDisplay, VoteRequest,
                 BallotRequest, VoteState, ProfileRequest };
enum : uint8_t { MapVote, NextVote, RestartVote, ShuffleVote };
struct Row
{
    bool present = false;
    uint8_t team = 0;
    uint16_t country = 0;
    uint32_t ping = 0, seconds = 0;
    int32_t score = 0, lives = 0;
    std::string name;
};
std::array<Row, MAXPLAYERS> rows{}, finalRows{};
std::array<uint32_t, MAXPLAYERS> generations{}, chatTime{};
std::array<uint64_t, MAXPLAYERS> joinOrder{};
std::array<netvote::Cooldown, MAXPLAYERS> cooldown{};
netvote::BasicVote<MAXPLAYERS> vote;
uint32_t generation = 0, rosterSent = 0, snapshotSequence = 0, snapshotTime = 0;
uint32_t snapshotMap = 0;
uint64_t order = 0;
bool chatTeam = false, haveSnapshot = false, frozen = false, forceScores = false;
bool voteRunning = false;
int32_t autoBalance = 1;
uint8_t voteAction = 0;
std::string voteArgument, voteLabel;
uint32_t voteUntil = 0;
unsigned voteYes = 0, voteNo = 0, voteEligible = 0;
uint32_t voteEligibleMask = 0, voteVotedMask = 0;

struct Writer
{
    std::vector<uint8_t> data;
    void byte(uint8_t value) { data.push_back(value); }
    void number(uint32_t value) { for (int shift = 24; shift >= 0; shift -= 8) byte(value >> shift); }
    void text(const std::string& value) { byte(uint8_t(value.size())); data.insert(data.end(), value.begin(), value.end()); }
};
struct Reader
{
    const std::vector<uint8_t>& data;
    size_t at = 0;
    bool valid = true;
    explicit Reader(const std::vector<uint8_t>& bytes) : data(bytes) {}
    uint8_t byte() { if (at == data.size()) { valid = false; return 0; } return data[at++]; }
    uint32_t number() { uint32_t value = 0; for (int i = 0; i < 4; ++i) value = (value << 8) | byte(); return value; }
    std::string text(size_t maximum)
    {
        size_t count = byte();
        if (!valid || count > maximum || count > data.size() - at) { valid = false; return {}; }
        std::string value(data.begin() + at, data.begin() + at + count); at += count;
        if (cleanText(value, maximum) != value) valid = false;
        return value;
    }
    bool done() const { return valid && at == data.size(); }
};

bool playerSlot(int player) { return unsigned(player) < MAXPLAYERS; }
bool member(int player) { return playerSlot(player) && connected(player) && generations[player]; }

void notice(const std::string& message)
{
    // The legacy HUD quote buffer is 178 bytes and G_AddUserQuote uses strcpy.
    // A long map vote can exceed that even though ordinary chat cannot.
    if (!dedicated()) G_AddUserQuote(cleanText(message,177).c_str());
    if (dedicated() || message.size() > 177) OSD_Printf("%s\n", message.c_str());
}

void announce(const std::string& message, int player = -1)
{
    Writer out;
    out.text(cleanText(message, 250));
    if (player < 0) { broadcast(ChatDisplay, out.data); notice(message); }
    else if (player == localPlayer() && !dedicated()) notice(message);
    else send(player, ChatDisplay, out.data);
}

std::array<netteams::Member, MAXPLAYERS> teamRoster(int excluded = -1)
{
    std::array<netteams::Member, MAXPLAYERS> result{};
    for (int player = 0; player < MAXPLAYERS; ++player)
    {
        result[player].active = member(player) && player != excluded;
        result[player].team = unsigned(g_player[player].pteam) & 1;
        result[player].joinOrder = joinOrder[player];
    }
    return result;
}

void team(int player, unsigned value)
{
    g_player[player].pteam = value;
    if (player == localPlayer()) ud.team = value;
    if (g_player[player].ps)
    {
        auto* ps = g_player[player].ps;
        ps->team = value;
        ps->palookup = g_player[player].pcolor = G_GetTeamPalette(value);
        if (playing(player) && unsigned(ps->i) < MAXSPRITES && sprite[ps->i].statnum != MAXSTATUS)
            sprite[ps->i].pal = ps->palookup;
    }
}

void balance(int excluded = -1)
{
    if (!server() || !autoBalance || !rules::teams()) return;
    for (int attempt = 0; attempt < MAXPLAYERS; ++attempt)
    {
        auto roster = teamRoster(excluded);
        int player = netteams::selectBalancePlayer(roster);
        if (player < 0) break;
        team(player, 1 - roster[player].team);
        announce(std::string(g_player[player].user_name) + " moved to " +
                 (g_player[player].pteam ? "RED" : "BLUE") + " for team balance");
    }
}

void updateRows()
{
    const uint32_t now = timerGetTicks();
    for (int player = 0; player < MAXPLAYERS; ++player)
    {
        Row& row = rows[player];
        row.present = member(player);
        if (!row.present) continue;
        row.name = cleanText(g_player[player].user_name, 31);
        row.team = unsigned(g_player[player].pteam) & 1;
        row.ping = std::min<uint32_t>(60000, ping(player));
        row.seconds = (now - connectedSince(player)) / 1000;
        row.score = rules::score(player);
        row.lives = rules::lives(player);
    }
}

std::vector<uint8_t> encodeRows()
{
    if (server()) updateRows();
    if (rules::intermission() && !frozen) { finalRows = rows; frozen = true; }
    const auto& source = frozen ? finalRows : rows;
    Writer out;
    out.number(mapSerial());
    out.byte(frozen);
    unsigned count = 0;
    for (const auto& row : source) count += row.present;
    out.byte(count);
    for (int player = 0; player < MAXPLAYERS; ++player)
    {
        const Row& row = source[player];
        if (!row.present) continue;
        out.byte(player); out.byte(row.team); out.number(row.country);
        out.number(row.ping); out.number(row.seconds); out.number(row.score);
        out.number(row.lives); out.text(row.name);
    }
    return out.data;
}

void sendRows(int recipient = -1)
{
    auto data = encodeRows();
    if (recipient < 0) broadcast(RosterMessage, data);
    else send(recipient, RosterMessage, data);
    rosterSent = timerGetTicks();
}

void sendVote(int recipient = -1)
{
    Writer out;
    uint32_t remaining = vote.remaining(timerGetTicks());
    out.number(remaining); out.byte(vote.yesCount()); out.byte(vote.noCount());
    out.byte(vote.eligibleCount());
    voteEligibleMask = voteVotedMask = 0;
    for (int i = 0; i < MAXPLAYERS; ++i)
    {
        if (vote.eligible(i,generations[i])) voteEligibleMask |= 1u << i;
        if (vote.hasVoted(i,generations[i])) voteVotedMask |= 1u << i;
    }
    out.number(voteEligibleMask); out.number(voteVotedMask); out.text(voteLabel);
    voteUntil = timerGetTicks() + remaining;
    voteRunning = remaining != 0;
    voteYes = vote.yesCount(); voteNo = vote.noCount(); voteEligible = vote.eligibleCount();
    if (recipient < 0) broadcast(VoteState, out.data);
    else send(recipient, VoteState, out.data);
}

void callVote(int player, uint8_t action, const std::string& argument)
{
    if (!member(player) || !playing(player)) return;
    if (vote.active()) { announce("A vote is already in progress", player); return; }
    if (rules::intermission()) { announce("Wait until the next map to call a vote", player); return; }
    uint32_t now = timerGetTicks();
    if (!cooldown[player].ready(now))
    {
        announce("Vote cooldown: " + std::to_string((cooldown[player].remaining(now) + 999) / 1000) + " seconds", player);
        return;
    }
    if (action > ShuffleVote || (action == MapVote && !rules::validMap(argument)) ||
        (action == ShuffleVote && !rules::teams()))
    { announce("That vote is not available", player); return; }
    std::array<uint32_t, MAXPLAYERS> eligible{};
    for (int i = 0; i < MAXPLAYERS; ++i) if (member(i) && playing(i)) eligible[i] = generations[i];
    if (!vote.start(now, eligible, player, generations[player])) return;
    cooldown[player].accept(now);
    voteAction = action; voteArgument = argument;
    static const char* labels[] = { "Map: ", "Next map", "Restart map", "Shuffle teams" };
    voteLabel = labels[action];
    if (action == MapVote) voteLabel += argument;
    announce(std::string(g_player[player].user_name) + " called a vote: " + voteLabel);
    sendVote();
}

void castVote(int player, bool yes)
{
    if (member(player) && vote.cast(timerGetTicks(), player, generations[player], yes)) sendVote();
}

void chat(int player, const std::string& text, bool toTeam)
{
    if (!member(player) || text.empty()) return;
    uint32_t now = timerGetTicks();
    if (chatTime[player] && uint32_t(now - chatTime[player]) < 500) return;
    chatTime[player] = now;
    if (toTeam && !rules::teams()) { announce("Team chat is available in team games", player); return; }
    const std::string message = (toTeam ? "(TEAM) " : "") +
        std::string(g_player[player].user_name) + ": " + text;
    if (!toTeam) { announce(message); return; }
    // Identity and recipients are selected exclusively from the server roster.
    Writer out; out.text(message);
    for (int i = 0; i < MAXPLAYERS; ++i)
        if (member(i) && g_player[i].pteam == g_player[player].pteam)
        {
            if (i == localPlayer() && !dedicated()) notice(message);
            else send(i, ChatDisplay, out.data);
        }
}

void changeProfile(int player, const std::string& name, unsigned requestedTeam)
{
    if (!member(player)) return;
    std::string normalized = normalizeName(player, name);
    Bstrncpyz(g_player[player].user_name, normalized.c_str(), sizeof(g_player[player].user_name));
    if (rules::teams()) { team(player, requestedTeam & 1); balance(); }
    sendRows();
}

int command(osdcmdptr_t args)
{
    const std::string name = args->name;
    if (name == "scores") { forceScores = !forceScores; return OSDCMD_OK; }
    if (!active() || playback()) { OSD_Printf("No live multiplayer connection\n"); return OSDCMD_OK; }
    if (name == "net_socialstatus")
    {
        if (server()) updateRows();
        for (int i = 0; i < MAXPLAYERS; ++i) if (rows[i].present)
        {
            const auto& row = rows[i];
            OSD_Printf("NET social slot=%d name=%s team=%u ping=%u seconds=%u score=%d lives=%d country=%c%c final=%d\n",
                i,row.name.c_str(),row.team,row.ping,row.seconds,row.score,row.lives,
                row.country ? row.country >> 8 : '-',row.country ? row.country & 255 : '-',frozen);
        }
        int32_t remaining = int32_t(voteUntil - timerGetTicks());
        bool running = voteRunning && remaining > 0 && remaining <= int32_t(netvote::DurationMs);
        OSD_Printf("NET vote active=%d yes=%u no=%u eligible=%u required=%u remaining=%u eligible_mask=%u voted_mask=%u\n",
            running,voteYes,voteNo,voteEligible,(voteEligible*51+99)/100,running ? unsigned(remaining) : 0,
            voteEligibleMask,voteVotedMask);
    }
    else if (name == "say" || name == "say_team")
    {
        if (!args->numparms) return OSDCMD_SHOWHELP;
        std::string text;
        for (int i = 0; i < args->numparms; ++i) { if (i) text += ' '; text += args->parms[i]; }
        sendChat(text.c_str(), name == "say_team");
    }
    else if (name == "vote")
    {
        if (args->numparms != 1 || (Bstrcasecmp(args->parms[0], "yes") && Bstrcasecmp(args->parms[0], "no")))
            return OSDCMD_SHOWHELP;
        castBallot(!Bstrcasecmp(args->parms[0], "yes"));
    }
    else if (name == "callvote")
    {
        if (!args->numparms) return OSDCMD_SHOWHELP;
        std::string action = args->parms[0], argument;
        if (action == "map" && args->numparms == 2) argument = cleanText(args->parms[1], 200);
        else if ((action == "next" || action == "restart" || action == "shuffle") && args->numparms == 1) {}
        else return OSDCMD_SHOWHELP;
        requestVote(action, argument);
    }
    return OSDCMD_OK;
}

palette_t color(uint32_t rgb)
{
    static std::unordered_map<uint32_t, palette_t> colors;
    auto found = colors.find(rgb);
    if (found != colors.end()) return found->second;
    int red = (rgb >> 16) & 255, green = (rgb >> 8) & 255, blue = rgb & 255;
    int nearest = 0, distance = 0x7fffffff;
    for (int i = 0; i < 256; ++i)
    {
        int r = int(uint8_t(curpalette[i].r)) - red, g = int(uint8_t(curpalette[i].g)) - green;
        int b = int(uint8_t(curpalette[i].b)) - blue;
        int d = r*r + g*g + b*b;
        if (d < distance) distance = d, nearest = i;
    }
    palette_t result = {char(red), char(green), char(blue), char(nearest)};
    colors.emplace(rgb, result);
    return result;
}

void rect(int x, int y, int width, int height, uint32_t rgb)
{
    auto c = color(rgb);
    for (int row = 0; row < height; ++row)
        drawlinergb(x << 12, (y + row) << 12, (x + width) << 12, (y + row) << 12, c);
}

void text(int x, int y, int scale, const std::string& value, uint32_t rgb = 0xffffff)
{
    for (unsigned char c : value)
    {
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 4; ++col)
                if (uint8_t(smalltextfont[c * 8 + row]) & (1 << (6 - col)))
                    rect(x + col * scale, y + row * scale, scale, scale, rgb);
        x += 4 * scale;
    }
}

void scoreboard()
{
    if (xdim < 320 || ydim < 200) return;
    const auto& source = frozen ? finalRows : rows;
    std::vector<int> order;
    for (int i = 0; i < MAXPLAYERS; ++i) if (source[i].present) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        if (rules::teams() && source[a].team != source[b].team) return source[a].team < source[b].team;
        return source[a].score > source[b].score;
    });
    int scale = std::max(1, std::min(xdim / 320, ydim / 220));
    int width = 316 * scale, height = (32 + int(order.size()) * 10) * scale;
    int x = (xdim - width) / 2, y = (ydim - height) / 2;
    // Common engine primitives support the software, OpenGL and Vulkan HUDs.
    const int32_t oldWx1 = wx1, oldWy1 = wy1, oldWx2 = wx2, oldWy2 = wy2;
    auto oldUpper = startumost, oldLower = startdmost;
    std::vector<int16_t> upper(xdim, 0), lower(xdim, ydim);
    wx1 = wy1 = 0; wx2 = xdim << 12; wy2 = ydim << 12;
    startumost = upper.data(); startdmost = lower.data();
    rect(x, y, width, height, 0x121820);
    text(x + 8*scale, y + 3*scale, scale, rules::intermission() ? "FINAL SCORES" : "SCOREBOARD", 0xffcc44);
    if (rules::intermission())
        text(x + 192*scale, y + 3*scale, scale, "NEXT MAP IN " + std::to_string(rules::remainingSeconds()));
    int rowY = y + 14*scale;
    text(x+8*scale,rowY,scale,"LOC"); text(x+24*scale,rowY,scale,"PING");
    text(x+48*scale,rowY,scale,"TIME"); text(x+80*scale,rowY,scale,"NAME");
    text(x+212*scale,rowY,scale,"TEAM"); text(x+244*scale,rowY,scale,"SCORE");
    text(x+284*scale,rowY,scale,"LIVES");
    rowY += 11*scale;
    for (int player : order)
    {
        const auto& row = source[player];
        const uint32_t rgb = player == localPlayer() ? 0xffcc44 : 0xffffff;
        auto pixels = netCountryFlag(row.country);
        for (int fy = 0; fy < 6; ++fy)
            for (int fx = 0; fx < 8; ++fx)
            {
                auto pixel = pixels + ((fy*2)*kNetCountryFlagWidth + fx*2)*3;
                rect(x+(9+fx)*scale,rowY+(1+fy)*scale,scale,scale,
                     (uint32_t(pixel[0])<<16)|(uint32_t(pixel[1])<<8)|pixel[2]);
            }
        text(x+24*scale,rowY,scale,std::to_string(row.ping),rgb);
        char elapsed[16]; std::snprintf(elapsed,sizeof(elapsed),"%u:%02u",std::min(999u,row.seconds/60),row.seconds%60);
        text(x+48*scale,rowY,scale,elapsed,rgb); text(x+80*scale,rowY,scale,row.name,rgb);
        text(x+212*scale,rowY,scale,rules::teams() ? (row.team ? "RED" : "BLUE") : "-", row.team ? 0xff7070 : 0x80b8ff);
        text(x+244*scale,rowY,scale,std::to_string(row.score),rgb);
        text(x+284*scale,rowY,scale,row.lives < 0 ? "-" : std::to_string(row.lives),rgb);
        rowY += 10*scale;
    }
    wx1 = oldWx1; wy1 = oldWy1; wx2 = oldWx2; wy2 = oldWy2;
    startumost = oldUpper; startdmost = oldLower;
}
} // namespace

std::string normalizeName(int player, const std::string& requested)
{
    std::array<std::string, MAXPLAYERS> names{};
    for (int i = 0; i < MAXPLAYERS; ++i) if (connected(i)) names[i] = g_player[i].user_name;
    return playerName(requested, playerSlot(player) ? player : 0, names);
}

std::vector<std::pair<uint8_t, std::vector<uint8_t>>> demoState()
{
    Writer out;
    uint32_t remaining = voteRunning && int32_t(voteUntil - timerGetTicks()) > 0 ? voteUntil - timerGetTicks() : 0;
    out.number(remaining); out.byte(voteYes); out.byte(voteNo); out.byte(voteEligible);
    out.number(voteEligibleMask); out.number(voteVotedMask); out.text(voteLabel);
    return {{RosterMessage, encodeRows()}, {VoteState, out.data}};
}

void init()
{
    rows = {}; finalRows = {}; generations = {}; chatTime = {}; joinOrder = {}; cooldown = {};
    vote = {}; generation = rosterSent = snapshotSequence = snapshotTime = snapshotMap = 0; order = 0;
    chatTeam = haveSnapshot = frozen = forceScores = false;
    voteRunning = false;
    voteLabel.clear(); voteArgument.clear(); voteUntil = 0;
    voteYes = voteNo = voteEligible = 0;
    voteEligibleMask = voteVotedMask = 0;
    netLagReset();
}

void join(int player)
{
    if (!server() || !playerSlot(player)) return;
    generations[player] = ++generation;
    if (!generations[player]) generations[player] = ++generation;
    joinOrder[player] = ++order;
    cooldown[player] = {}; chatTime[player] = 0;
    std::string name = normalizeName(player, g_player[player].user_name);
    Bstrncpyz(g_player[player].user_name, name.c_str(), sizeof(g_player[player].user_name));
#ifndef NETCODE_DISABLE
    rows[player].country = peer(player) ? netCountryLookupIPv6(peer(player)->address.host.s6_addr) : 0;
#endif
    if (rules::teams())
    {
        unsigned count[2] = {};
        for (int i = 0; i < MAXPLAYERS; ++i) if (i != player && member(i)) ++count[unsigned(g_player[i].pteam)&1];
        team(player, autoBalance ? (count[1] < count[0] ? 1 : 0) : unsigned(g_player[player].pteam)&1);
    }
    sendRows();
    if (vote.active()) sendVote(player);
}

void leave(int player)
{
    if (!server() || !playerSlot(player)) return;
    generations[player] = 0; rows[player].present = false;
    balance(player); sendRows();
    if (vote.active()) sendVote();
}

void ready(int player)
{
    if (!server() || !member(player)) return;
    sendRows(player);
    if (vote.active()) sendVote(player);
}

void shuffle()
{
    if (!server() || !rules::teams()) return;
    auto assignments = netteams::shuffleTeams(teamRoster(), timerGetTicks());
    for (int i = 0; i < MAXPLAYERS; ++i) if (assignments[i] >= 0) team(i, assignments[i]);
    announce("Teams shuffled"); sendRows();
}

void tick()
{
    if (!active()) return;
    if (server())
    {
        uint32_t now = timerGetTicks();
        if (uint32_t(now - rosterSent) >= 1000) { balance(); sendRows(); }
        if (vote.active() && rules::intermission())
        {
            vote.cancel(true); sendVote(); announce("Vote cancelled: match ended");
        }
        auto result = vote.poll(now);
        if (result != netvote::Result::NoResult)
        {
            sendVote();
            announce(result == netvote::Result::Passed ? "Vote passed" : "Vote failed");
            if (result == netvote::Result::Passed)
            {
                if (voteAction == MapVote) queueMap(voteArgument);
                else if (voteAction == NextVote) rules::nextMap();
                else if (voteAction == RestartVote) queueMap(mapName(), true);
                else shuffle();
            }
        }
    }
    if (rules::intermission() && !frozen)
    {
        if (server()) { updateRows(); sendRows(); }
        finalRows = rows; frozen = true;
    }
    else if (!rules::intermission()) frozen = false;
}

void profile()
{
    if (!active() || playback()) return;
    if (server()) changeProfile(localPlayer(), szPlayerName, ud.team);
    else
    {
        Writer out; out.text(cleanText(szPlayerName,31)); out.byte(unsigned(ud.team)&1);
        send(-1, ProfileRequest, out.data);
    }
}

void sendChat(const char* message, bool toTeam)
{
    if (!active() || playback() || !message) return;
    std::string value = cleanText(message, 120);
    if (value.empty()) return;
    if (server() && dedicated())
    {
        if (!toTeam) announce("SERVER: " + value);
        else notice("The dedicated server has no team; use say for public announcements");
    }
    else if (server()) chat(localPlayer(), value, toTeam);
    else { Writer out; out.byte(toTeam); out.text(value); send(-1, ChatRequest, out.data); }
}

void chatInput()
{
    if (KB_KeyPressed(sc_Tab)) { chatTeam = !chatTeam; KB_ClearKeyDown(sc_Tab); }
    if (!rules::teams()) chatTeam = false;
    minitext(12, 142, chatTeam ? "TEAM CHAT (TAB: PUBLIC)" : "PUBLIC CHAT (TAB: TEAM)", 0, 2|8|16);
    int result = I_EnterText(typebuf, 120, 0);
    mpgametext(12<<16,155<<16,typebuf,0,2|8|16|ROTATESPRITE_FULL16,0,TEXT_LITERALESCAPE);
    if (result)
    {
        if (result > 0) sendChat(typebuf, chatTeam);
        typebuf[0] = 0;
        g_player[myconnectindex].ps->gm &= ~(MODE_TYPE|MODE_SENDTOWHOM);
        KB_ClearKeyDown(sc_Enter);
    }
}

void input()
{
    if (!active() || playback() || dedicated() || !playerSlot(localPlayer())) return;
    int32_t left = int32_t(voteUntil - timerGetTicks());
    uint32_t bit = 1u << localPlayer();
    if (!voteRunning || left <= 0 || left > int32_t(netvote::DurationMs) || !(voteEligibleMask & bit) || (voteVotedMask & bit)) return;
    auto ps = g_player[localPlayer()].ps;
    if (!ps || (ps->gm & (MODE_MENU|MODE_TYPE|MODE_SENDTOWHOM)) || SHIFTS_IS_PRESSED || ALT_IS_PRESSED || WIN_IS_PRESSED) return;
    bool yes = KB_UnBoundKeyPressed(sc_F1);
    bool no = KB_UnBoundKeyPressed(sc_F2);
    if (!yes && !no) return;
    KB_ClearKeyDown(sc_F1); KB_ClearKeyDown(sc_F2);
    castBallot(yes);
}

void requestVote(const std::string& action, const std::string& map)
{
    if (!active() || playback()) return;
    uint8_t kind;
    if (action == "map") kind = MapVote;
    else if (action == "next") kind = NextVote;
    else if (action == "restart") kind = RestartVote;
    else if (action == "shuffle") kind = ShuffleVote;
    else return;
    const auto argument = cleanText(map,200);
    if (server()) callVote(localPlayer(),kind,argument);
    else { Writer out; out.byte(kind); out.text(argument); send(-1,VoteRequest,out.data); }
}

void castBallot(bool yes)
{
    if (!active() || playback()) return;
    if (server()) castVote(localPlayer(),yes);
    else
    {
        send(-1,BallotRequest,{uint8_t(yes)});
        if (playerSlot(localPlayer())) voteVotedMask |= 1u << localPlayer();
    }
}

bool handle(int sender, uint8_t type, const std::vector<uint8_t>& payload)
{
    if (type < RosterMessage || type > ProfileRequest) return false;
    Reader in(payload);
    if (server())
    {
        if (!member(sender)) return true;
        if (type == ChatRequest)
        {
            uint8_t toTeam = in.byte(); auto value = in.text(120);
            if (in.done() && toTeam <= 1) chat(sender, value, toTeam);
        }
        else if (type == VoteRequest)
        {
            uint8_t action = in.byte(); auto argument = in.text(200);
            if (in.done()) callVote(sender, action, argument);
        }
        else if (type == BallotRequest)
        {
            uint8_t yes = in.byte(); if (in.done() && yes <= 1) castVote(sender, yes);
        }
        else if (type == ProfileRequest)
        {
            auto name = in.text(31); unsigned desiredTeam = in.byte();
            if (in.done() && desiredTeam < 2) changeProfile(sender,name,desiredTeam);
        }
        return true;
    }
    if (sender != -1) return true;
    if (type == ChatDisplay)
    {
        auto value = in.text(250); if (in.done()) notice(value);
    }
    else if (type == RosterMessage)
    {
        std::array<Row, MAXPLAYERS> incoming{};
        uint32_t serial = in.number();
        uint8_t final = in.byte();
        unsigned count = in.byte();
        if (count > MAXPLAYERS || final > 1 || serial != mapSerial()) return true;
        for (unsigned i = 0; i < count; ++i)
        {
            unsigned slot = in.byte();
            if (!in.valid || slot >= MAXPLAYERS || incoming[slot].present) return true;
            Row& row = incoming[slot]; row.present = true; row.team = in.byte();
            uint32_t country = in.number(); row.country = country;
            row.ping = in.number(); row.seconds = in.number(); row.score = int32_t(in.number());
            row.lives = int32_t(in.number()); row.name = in.text(31);
            if (row.team > 1 || country > 65535 || row.ping > 60000 || row.name.empty()) return true;
        }
        if (in.done())
        {
            rows = std::move(incoming);
            if (final) { finalRows = rows; frozen = true; }
            for (int i = 0; i < MAXPLAYERS; ++i) if (rows[i].present)
            {
                Bstrncpyz(g_player[i].user_name,rows[i].name.c_str(),sizeof(g_player[i].user_name));
                if (rules::teams()) team(i,rows[i].team);
            }
        }
    }
    else if (type == VoteState)
    {
        uint32_t remaining = in.number(); unsigned yes = in.byte(), no = in.byte(), eligible = in.byte();
        uint32_t eligibleMask = in.number(), votedMask = in.number();
        auto label = in.text(250);
        if (in.done() && remaining <= netvote::DurationMs && eligible <= MAXPLAYERS && yes+no <= eligible &&
            eligibleMask < (1u << MAXPLAYERS) && !(votedMask & ~eligibleMask))
        {
            voteUntil = timerGetTicks() + remaining; voteYes = yes; voteNo = no;
            voteRunning = remaining != 0;
            voteEligible = eligible; voteLabel = std::move(label);
            voteEligibleMask = eligibleMask; voteVotedMask = votedMask;
        }
    }
    return true;
}

void snapshot(uint32_t sequence, unsigned pending, bool rateDelayed)
{
    if (snapshotMap != mapSerial())
    {
        snapshotMap = mapSerial(); haveSnapshot = false; frozen = false;
        netLagReset();
    }
    uint32_t gap = haveSnapshot ? sequence - snapshotSequence : 1;
    if (haveSnapshot && (gap == 0 || gap > 0x7fffffffu)) return;
    netLagometerSnapshot(ping(localPlayer()), int(std::min(128u,gap-1)), rateDelayed);
    netLagometerCommandAck(pending);
    snapshotSequence = sequence; snapshotTime = timerGetTicks(); haveSnapshot = true;
    netLagometerDisconnect(false);
}

void frame(int timingOffsetMs) { netLagFrame(timingOffsetMs); }

void draw(bool show)
{
    if (!active() || dedicated()) return;
    if (!rules::intermission()) frozen = false;
    if (client() && !playback() && haveSnapshot)
    {
        uint32_t age = timerGetTicks() - snapshotTime;
        netLagFrame(int(std::min(age,60000u)) - int(1000/TickRate));
        netLagometerDisconnect(age > 1000 && inGame());
        netLagDraw();
    }
    if (show || forceScores || rules::intermission()) scoreboard();
    int32_t left = int32_t(voteUntil - timerGetTicks());
    if (left <= 0) voteRunning = false;
    if (voteRunning && left > 0 && left <= int32_t(netvote::DurationMs))
    {
        minitext(8,25,voteLabel.c_str(),0,2|8|16);
        char label[100];
        uint32_t bit = playerSlot(localPlayer()) ? 1u << localPlayer() : 0;
        const char* prompt = !(voteEligibleMask & bit) ? "SPECTATING VOTE" :
            (voteVotedMask & bit) ? "VOTE RECORDED" : "F1 YES / F2 NO";
        std::snprintf(label,sizeof(label),"YES %u  NO %u  NEED %u/%u  %ds  %s",voteYes,voteNo,
                      (voteEligible*51+99)/100,voteEligible,(left+999)/1000,prompt);
        minitext(8,33,label,0,2|8|16);
    }
}

void registerCommands()
{
    static osdcvardata_t vars[] = {
        {"cl_lagometer","show network latency and snapshot loss",&gNetLagometer,CVAR_BOOL,0,1},
        {"net_lagometer","show network latency and snapshot loss",&gNetLagometer,CVAR_BOOL,0,1},
        {"sv_autoteambalance","keep team sizes within one player",&autoBalance,CVAR_BOOL,0,1},
    };
    for (auto& var : vars) OSD_RegisterCvar(&var, osdcmd_cvar_set);
    OSD_RegisterFunction("say","say <message>: public chat",command);
    OSD_RegisterFunction("say_team","say_team <message>: chat with your team",command);
    OSD_RegisterFunction("scores","scores: toggle multiplayer scoreboard",command);
    OSD_RegisterFunction("net_socialstatus","net_socialstatus: print trusted scoreboard and current vote",command);
    OSD_RegisterFunction("callvote","callvote <map MAP|next|restart|shuffle>: start a 30-second vote",command);
    OSD_RegisterFunction("vote","vote <yes|no>: cast your ballot",command);
}
} }
