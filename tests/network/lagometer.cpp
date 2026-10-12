#include "netlag.h"
#include <climits>
#include <cstdio>
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); std::exit(1); } } while (0)
int main()
{
    netLagReset();
    const auto& history = netLagHistory();
    CHECK(!history.active && !history.interrupted && !history.frameCount && !history.snapshotCount);
    netLagFrame(-20); netLagFrame(30);
    CHECK(history.frameMs[0] == -20 && history.frameMs[1] == 30);
    netLagFrame(INT_MIN); netLagFrame(INT_MAX);
    CHECK(history.frameMs[2] == -60000 && history.frameMs[3] == 60000);
    for (unsigned i = 4; i < 256; ++i) netLagFrame(int(i));
    CHECK(history.frameCount == 256 && history.frameMs[0] == 128 && history.frameMs[127] == 255);
    netLagometerSnapshot(75, 3, true);
    CHECK(history.active && history.snapshotCount == 4);
    for (unsigned i = 0; i < 3; ++i) CHECK(history.snapshots[i].pingMs == -1 && !history.snapshots[i].rateDelayed);
    CHECK(history.snapshots[3].pingMs == 75 && history.snapshots[3].rateDelayed);
    netLagometerSnapshot(INT_MAX, INT_MAX);
    CHECK(history.snapshotCount == 133 && history.snapshots[132 & 127].pingMs == 60000);
    for (unsigned i = 5; i < 132; ++i) CHECK(history.snapshots[i & 127].pingMs == -1);
    netLagometerSnapshot(INT_MIN, INT_MIN);
    CHECK(history.snapshotCount == 134 && history.snapshots[133 & 127].pingMs == 0);
    netLagometerCommandAck(17); netLagometerDisconnect(true);
    CHECK(history.pendingCommands == 17 && history.interrupted);
    netLagometerDisconnect(false); CHECK(!history.interrupted);
    netLagReset(); CHECK(!history.active && !history.pendingCommands && !history.frameCount && !history.snapshotCount);
    std::puts("PASS: lagometer frame/RTT/loss samples, flags, bounded gaps, ring wrap and reset");
}
