// Messages between the client and the witness over the Noise channel (engine/witnessnet.cpp,
// witness/witness.cpp). A transport message is a u8 type and a payload of varints and bytes.
//
// Handshake payloads: the witness's (message 2) is u8 version, 16 session id, 16 nonce, u64
// server time in us, u32 seed; the client's (message 3) is a str with its build.
//
// client -> witness
//   RUNSTART  var run id, var kind (0 map attempt, 1 life, 2 the next run of a life), var start tick,
//             and for kind 2 the run id it continues
//   DATA      var run id, the replay file's next bytes
//   TICK      var run id, var tick, u32 combined state hash of that tick
//   STOP      var run id, str reason (the rest of the run is not witnessed)
// witness -> client
//   SEED      u32 seed for the client's next map start
//   SIGN      var run id, a sIGN chunk payload
//   RUNERROR  var run id, str reason (the witness gave up on the run)
//   RUNSTOP   var run id, str reason (the witness stopped watching the run, which isn't at fault)

#ifndef __WITNESSPROTO_H__
#define __WITNESSPROTO_H__

enum
{
    WITNESSPROTO = 1,
    WM_RUNSTART = 1, WM_DATA, WM_TICK, WM_STOP,
    WM_SEED = 101, WM_SIGN, WM_RUNERROR, WM_RUNSTOP
};
enum { WITNESSRUN_MAP = 0, WITNESSRUN_LIFE = 1, WITNESSRUN_CHAIN = 2 };
// ticks between TICK messages (250 ms): a replay file keeps the state hash of every 50th tick
enum { WITNESSTICKSTEP = 50 };
enum { WITNESSPORT = 28790 };

#endif
