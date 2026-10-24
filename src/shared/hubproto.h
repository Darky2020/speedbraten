// Messages between the client and the hub over the Noise channel (prologue "speedbraten/1 hub").
// Same framing and payload encoding as witnessproto.h: varints (LEB128) and strs (var length,
// bytes). server/hub/proto.go has the same numbers.
//
// A key request (an empty first frame) gets "SBRH", u8 1, the hub's channel public key.
// Handshake payloads: the hub's (message 2) is u8 HUBPROTO; the client's (message 3) is its
// build, as plain bytes.
//
// Players are named by a var id that lasts for their connection. Lobby id 0 means none.
// svar is a zigzag varint (game modes go below zero). kind is the run kind of the map being
// played: "flagrun", "race", "campaign", "dmsp", or "" for anything else.
//
// client -> hub
//   HELLO        str name the player wants (sent once, first)
//   SETNAME      str name
//   STATUS       var state (HUBSTATE_*), str map, svar mode, str kind
//   LOBBYCREATE  str map, svar mode, str kind (the creator is the host)
//   LOBBYJOIN    var lobby id
//   LOBBYLEAVE
//   LOBBYMAP     str map, svar mode, str kind (host only)
//   CHAT         var scope (HUBCHAT_*), str text
//   REPORT       var player id, str reason
//   POS          svar x, svar y, svar z (1/16 units), var yaw, var pitch (0-65535 over the circle /
//                -90..90 degrees), var ghost flags (fpsgame/inputs.h ghostsample) - 20 a second
//   FINISH       str map, svar mode, str kind, var run time in ms (the client's own timing)
// hub -> client
//   WELCOME      var your id, str your name ("" while you have none), str why the name you
//                asked for was refused ("" if it wasn't)
//   WITNESS      str host, var port, 32 channel key, 32 signing key
//   PLAYER       var id, str name, var state, str map, svar mode, var lobby id, str kind
//                (new or changed)
//   GONE         var id
//   LOBBY        var id, var host id, str map, svar mode, var count, count x var member id,
//                str kind
//   LOBBYGONE    var id
//   CHATFROM     var scope, var from id, str name, str text
//   NOTICE       str text
//   POSFROM      var player id, then a POS payload (a lobby member on the same map and mode)
//   RESULT       str name, str map, svar mode, str kind, var run time in ms, var record (1) or PB (0),
//                var the time it beat + 1 (0: the first verified time) - a witnessed PB or record
//   FINISHED     var player id, str name, str map, svar mode, str kind, var run time in ms (a lobby
//                member finished a run; not sent back to the one who finished)

#ifndef __HUBPROTO_H__
#define __HUBPROTO_H__

enum
{
    HUBPROTO = 1,
    HM_HELLO = 1, HM_SETNAME, HM_STATUS, HM_LOBBYCREATE, HM_LOBBYJOIN, HM_LOBBYLEAVE, HM_LOBBYMAP, HM_CHAT, HM_REPORT, HM_FINISH, HM_POS,
    HM_WELCOME = 101, HM_WITNESS, HM_PLAYER, HM_GONE, HM_LOBBY, HM_LOBBYGONE, HM_CHATFROM, HM_NOTICE, HM_FINISHED, HM_RESULT, HM_POSFROM
};
enum { HUBSTATE_MENU = 0, HUBSTATE_PLAYING, HUBSTATE_RUN };
enum { HUBCHAT_GLOBAL = 0, HUBCHAT_LOBBY };
enum { HUBPORT = 28791 };

#endif
