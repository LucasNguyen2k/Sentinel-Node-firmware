// DataSocket: long-lived data/command socket on raw lwIP (FW-10, FW-20, FW-09)
// @extract firmware/sentinel_node/ezsn_hal.cpp: DataSocket::connect DataSocket::connected DataSocket::available DataSocket::read DataSocket::write DataSocket::stop DataSocket::fill
#include "minitest.h"
#include "fw_env.h"
#include "extracted.inc"

static bool logged(const char *text)
{
    for (auto &l : Serial.lines) if (l.find(text) != std::string::npos) return true;
    return false;
}

TEST(connect_resolves_and_connects, "FW-10")
{
    fake_reset_all();
    DataSocket s;
    CHECK_EQ(s.connect("192.168.100.1", 4044), 1);
    CHECK_EQ((int)s.connected(), 1);
}

TEST(connect_failures, "FW-10,FW-20")
{
    fake_reset_all();
    DataSocket s;
    fakehal.resolve_ok = false;
    CHECK_EQ(s.connect("edge.local", 4044), 0);
    CHECK_EQ((int)s.connected(), 0);
    fakehal.resolve_ok = true; fakehal.connect_result = -1;
    CHECK_EQ(s.connect("192.168.100.1", 4044), 0);
    CHECK_EQ((int)s.connected(), 0);
    CHECK_EQ(fakehal.closed, 0);                   // nothing to close
}

TEST(idle_socket_stays_open, "FW-10,FW-20")
{
    // regression test for 1.1.7: an idle socket (no data waiting) must never be treated as closed
    fake_reset_all();
    DataSocket s;
    s.connect("192.168.100.1", 4044);
    for (int i = 0; i < 1000; i++) CHECK_EQ(s.available(), 0);
    CHECK_EQ((int)s.connected(), 1);
    CHECK_EQ(fakehal.closed, 0);
}

TEST(receives_commands_across_chunks, "FW-10")
{
    fake_reset_all();
    DataSocket s;
    s.connect("192.168.100.1", 4044);
    std::string big = "CMD#c1 TIME 1791500000\n" + std::string(600, 'x');
    fakehal.rx.push_back(big);
    std::string got;
    uint8_t buf[100];
    for (int guard = 0; guard < 50 && got.size() < big.size(); guard++) {
        int n = s.read(buf, sizeof(buf));
        got.append((const char *)buf, n);
    }
    CHECK_EQ(got, big);
    CHECK_EQ((int)s.connected(), 1);
}

TEST(peer_close_detected_once, "FW-20")
{
    fake_reset_all();
    DataSocket s;
    s.connect("192.168.100.1", 4044);
    fakehal.recv_codes.push_back(-1);              // orderly close by the server
    CHECK_EQ((int)s.connected(), 0);
    CHECK_EQ(fakehal.closed, 1);
    CHECK(logged("closed by server"));
    s.stop();
    CHECK_EQ(fakehal.closed, 1);                   // stop() is idempotent
    uint8_t b[4];
    CHECK_EQ(s.read(b, 4), 0);
}

TEST(socket_error_closes_with_reason, "FW-20")
{
    fake_reset_all();
    DataSocket s;
    s.connect("192.168.100.1", 4044);
    fakehal.recv_codes.push_back(-104);            // -ECONNRESET
    CHECK_EQ(s.available(), 0);
    CHECK_EQ((int)s.connected(), 0);
    CHECK(logged("socket error 104"));
}

TEST(write_sends_exact_bytes, "FW-10")
{
    fake_reset_all();
    DataSocket s;
    s.connect("192.168.100.1", 4044);
    const char *pkt = "21.5,55.0,400,1791500000,1,-1.0,-40.0,-55.0,-1,0,83$";
    CHECK_EQ(s.write((const uint8_t *)pkt, strlen(pkt)), strlen(pkt));
    CHECK_EQ(fakehal.sent.size(), (size_t)1);
    CHECK_EQ(fakehal.sent[0], std::string(pkt));
}

TEST(write_failure_closes_and_reports, "FW-20")
{
    fake_reset_all();
    DataSocket s;
    s.connect("192.168.100.1", 4044);
    fakehal.send_fail_after = 0; fakehal.so_error = 11;
    CHECK_EQ(s.write((const uint8_t *)"abc", 3), (size_t)0);
    CHECK_EQ((int)s.connected(), 0);
    CHECK(logged("send failed, so_error=11"));
    CHECK_EQ(s.write((const uint8_t *)"abc", 3), (size_t)0);   // closed socket: nothing sent
    CHECK_EQ(fakehal.sent.size(), (size_t)0);
}

TEST(reconnect_closes_previous_socket, "FW-20")
{
    fake_reset_all();
    DataSocket s;
    s.connect("192.168.100.1", 4044);
    fakehal.connect_result = 9;
    s.connect("192.168.100.1", 4044);
    CHECK_EQ(fakehal.closed, 1);
    CHECK_EQ(fakehal.last_closed, 7);              // only our own old socket, never another number
}

MINITEST_MAIN
