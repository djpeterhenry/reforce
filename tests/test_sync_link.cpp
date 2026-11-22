#include <doctest/doctest.h>

#include "sync/LinkClock.hpp"

using namespace reforce::sync;

TEST_CASE("link: pulse is 1 on the beat and decays like the onset BEAT") {
    CHECK(linkPulse(8.0, 120.0) == doctest::Approx(1.0));
    // Half a beat at 120 BPM = 0.25 s -> exp(-0.25 / 0.12).
    CHECK(linkPulse(8.5, 120.0) == doctest::Approx(std::exp(-0.25 / 0.12)));
    CHECK(linkPulse(-0.5, 120.0) == doctest::Approx(std::exp(-0.25 / 0.12)));  // before the session origin
    CHECK(linkPulse(3.0, 0.0) == 0.0);
}

TEST_CASE("link: boundary crossings for beats and bars") {
    CHECK(crossedBoundary(3.98, 4.01, 1.0));
    CHECK(crossedBoundary(3.98, 4.01, 4.0));
    CHECK_FALSE(crossedBoundary(4.01, 4.5, 1.0));
    CHECK(crossedBoundary(4.9, 5.0, 1.0));
    CHECK_FALSE(crossedBoundary(4.9, 5.0, 4.0));
    CHECK(crossedBoundary(-0.1, 0.05, 4.0));  // negative beats (session start) count too
    CHECK_FALSE(crossedBoundary(8.2, 7.9, 1.0));  // backward jump never counts
    CHECK_FALSE(crossedBoundary(1.0, 2.0, 0.0));
}

TEST_CASE("link: beats to seconds") {
    CHECK(beatsToSeconds(4, 120) == doctest::Approx(2.0));
    CHECK(beatsToSeconds(8, 0) == 0.0);
}

TEST_CASE("link: a disabled clock reports nothing") {
    LinkClock c(4.0, 128.0);
    const LinkSnapshot s = c.sample(0.0);
    CHECK_FALSE(s.enabled);
    CHECK_FALSE(s.on);
    CHECK(s.peers == 0);
    CHECK(s.bpm == 0.0);
    CHECK(s.quantum == 4.0);
}

TEST_CASE("link follow: joins only after another app announced steadily, leaves when it goes") {
    LinkFollower f;
    CHECK(f.update(0.0, 0) == 0);  // nothing heard: stay off, whatever the time
    CHECK(f.update(100.0, 0) == 0);
    for (double t = 100.0; t < 101.4; t += 0.25) {
        f.onAnnouncement(t);
        CHECK(f.update(t, 0) == 0);  // not yet kFollowJoinDelay
    }
    f.onAnnouncement(101.5);
    CHECK(f.update(101.5, 0) == +1);  // heard for 1.5 s: join
    CHECK(f.joined());
    f.onAnnouncement(101.6);  // our own announcements while joined are ignored
    CHECK(f.update(102.0, 0) == 0);  // session forming
    CHECK(f.update(102.5, 1) == 0);
    CHECK(f.update(200.0, 2) == 0);
    CHECK(f.update(200.1, 0) == -1);  // the last peer left: leave at once
    CHECK_FALSE(f.joined());
    CHECK(f.update(200.2, 0) == 0);
}

TEST_CASE("link follow: a single stray announcement never joins; a session that never forms is left") {
    LinkFollower f;
    f.onAnnouncement(10.0);
    CHECK(f.update(11.6, 0) == 0);  // no announcement for > 1 s
    CHECK(f.update(20.0, 0) == 0);
    for (double t = 30.0; t <= 31.5; t += 0.25) f.onAnnouncement(t);
    CHECK(f.update(31.5, 0) == +1);
    CHECK(f.update(36.4, 0) == 0);
    CHECK(f.update(36.5, 0) == -1);  // no peer within kFollowFormGrace
}

TEST_CASE("link follow: a follower clock only listens until another app appears") {
    LinkClock c(4.0, 120.0, true);
    c.setEnabled(true);
    const LinkSnapshot s = c.sample(0.0);
    CHECK(s.enabled);
    CHECK_FALSE(s.on);
    CHECK(s.peers == 0);
    c.setEnabled(false);
    CHECK_FALSE(c.sample(0.0).enabled);
}
