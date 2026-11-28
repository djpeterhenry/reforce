#include <doctest/doctest.h>

#include "web/Guests.hpp"

using reforce::web::parseCheckins;

TEST_CASE("guests: check-ins become dancer scene files with a name tag") {
    const char* body = R"({"seq":7,"checkins":[
        {"seq":6,"at":1,"id":"abc123","name":"Sam \"The Man\"","dancer":{"Style":"disco","HairStyle":"afro",
         "Skin":"#5a3825","Build":1.1,"Lashes":1,"Freckles":false,"Bad Key":"x","Name":"evil"}},
        {"seq":7,"at":2,"id":"def456","name":"Alex","dancer":{"Style":"pop"}},
        {"seq":8,"id":"broken"}]})";
    std::int64_t latest = 0;
    std::string err;
    const auto got = parseCheckins(body, &latest, &err);
    REQUIRE(got);
    CHECK(latest == 7);
    REQUIRE(got->size() == 2);
    const auto& a = (*got)[0];
    CHECK(a.seq == 6);
    CHECK(a.id == "abc123");
    CHECK(a.name == "Sam The Man");  // quotes dropped (the tag goes into Name="...")
    CHECK(a.sceneText.find("Vers=100\n") == 0);
    CHECK(a.sceneText.find("Style=\"disco\"") != std::string::npos);
    CHECK(a.sceneText.find("Build=\"1.1\"") != std::string::npos);
    CHECK(a.sceneText.find("Lashes=\"1\"") != std::string::npos);
    CHECK(a.sceneText.find("Freckles=\"0\"") != std::string::npos);
    CHECK(a.sceneText.find("Bad Key") == std::string::npos);
    CHECK(a.sceneText.find("evil") == std::string::npos);
    CHECK(a.sceneText.find("Name=\"Sam The Man\"") != std::string::npos);
    CHECK((*got)[1].name == "Alex");
}

TEST_CASE("guests: not the expected JSON") {
    std::string err;
    CHECK_FALSE(parseCheckins("<html>", nullptr, &err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(parseCheckins(R"({"seq":1})", nullptr, &err));
}

#include <httplib.h>

#include <atomic>
#include <mutex>
#include <chrono>
#include <thread>

TEST_CASE("guests: the feed polls the service and hands over each check-in once") {
    httplib::Server srv;
    std::atomic<int> polls{0};
    std::string lastAfter;
    srv.Get("/api/checkins", [&](const httplib::Request& req, httplib::Response& res) {
        ++polls;
        lastAfter = req.get_param_value("after");
        const bool first = lastAfter == "-1";
        res.set_content(first ? R"({"seq":3,"checkins":[{"seq":3,"id":"g1","name":"Sam","dancer":{"Style":"pop"}}]})"
                              : R"({"seq":3,"checkins":[]})",
                        "application/json");
    });
    const int port = srv.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::thread th([&] { srv.listen_after_bind(); });

    reforce::web::GuestFeed feed;
    feed.configure(true, "http://127.0.0.1:" + std::to_string(port) + "/", 1.0);
    std::vector<reforce::web::GuestArrival> got;
    for (int i = 0; i < 100 && (got.empty() || polls < 2); ++i) {
        auto t = feed.take().arrivals;
        got.insert(got.end(), t.begin(), t.end());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(got.size() == 1);
    if (!got.empty()) CHECK(got[0].name == "Sam");
    CHECK(polls >= 2);
    CHECK(lastAfter == "3");  // the second poll asks only for what's new
    CHECK(feed.status() == "ok");
    CHECK(feed.arrivals() == 1);
    feed.configure(false, "");
    srv.stop();
    th.join();
}

TEST_CASE("guests: a check-in is handed over once, across switching off and on and a restart") {
    httplib::Server srv;
    std::atomic<int> polls{0};
    srv.Get("/api/checkins", [&](const httplib::Request& req, httplib::Response& res) {
        ++polls;
        const long after = std::stol(req.get_param_value("after"));
        res.set_content(after < 3 ? R"({"seq":3,"checkins":[{"seq":3,"id":"g1","name":"Sam","dancer":{"Style":"pop"}}]})"
                                  : R"({"seq":3,"checkins":[]})",
                        "application/json");
    });
    const int port = srv.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::thread th([&] { srv.listen_after_bind(); });
    const std::string url = "http://127.0.0.1:" + std::to_string(port);
    const std::string memory = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                               "/reforce-guests-seen-test-" + std::to_string(port) + ".txt";
    std::remove(memory.c_str());
    auto collect = [&](reforce::web::GuestFeed& feed, int minPolls) {
        std::vector<reforce::web::GuestArrival> got;
        const int start = polls;
        for (int i = 0; i < 100 && polls < start + minPolls; ++i) {
            auto t = feed.take().arrivals;
            got.insert(got.end(), t.begin(), t.end());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        auto t = feed.take().arrivals;
        got.insert(got.end(), t.begin(), t.end());
        return got;
    };
    {
        reforce::web::GuestFeed feed(memory);
        feed.configure(true, url, 1.0);
        CHECK(collect(feed, 2).size() == 1);
        feed.configure(false, url, 1.0);  // off and on again: nothing new
        feed.configure(true, url, 1.0);
        CHECK(collect(feed, 2).empty());
    }
    {
        reforce::web::GuestFeed feed(memory);  // a restart: remembers what was handed over
        feed.configure(true, url, 1.0);
        CHECK(collect(feed, 2).empty());
    }
    std::remove(memory.c_str());
    srv.stop();
    th.join();
}

TEST_CASE("guests: messages come with the check-ins, text as sent (emoji too)") {
    const char* body = R"({"seq":2,"checkins":[],"msgSeq":9,"messages":[
        {"seq":8,"at":1,"id":"abc123","name":"Sam","text":"Love this track 🔥 turn it up!"},
        {"seq":9,"at":2,"id":"def456","name":"Alex","text":"hi\nthere"},
        {"seq":10,"id":"broken"}]})";
    std::string err;
    const auto p = reforce::web::parseGuestPoll(body, &err);
    REQUIRE(p);
    CHECK(p->latest == 2);
    CHECK(p->msgLatest == 9);
    REQUIRE(p->messages.size() == 2);
    CHECK(p->messages[0].id == "abc123");
    CHECK(p->messages[0].text == "Love this track \xF0\x9F\x94\xA5 turn it up!");
    CHECK(p->messages[1].text == "hi there");  // no control characters
}

#include <filesystem>
#include <fstream>

#include "web/GuestBook.hpp"

namespace {
std::filesystem::path freshDir(const char* name) {
    const auto d = std::filesystem::temp_directory_path() / ("reforce-test-" + std::string(name));
    std::filesystem::remove_all(d);
    return d;
}
}  // namespace

TEST_CASE("guest book: each guest's latest dancer is kept, named after them, apart by id") {
    using reforce::web::GuestBook;
    const auto dir = freshDir("guestbook");
    {
        GuestBook book(dir);
        CHECK(book.guests().empty());
        CHECK(book.save("id-robin", "Robin", "Vers=100\nStyle=\"jazz\"\nName=\"Robin\"\n", 1000).empty());
        CHECK(book.save("id-sam1", "Sam", "Vers=100\nStyle=\"house\"\nName=\"Sam\"\n", 2000).empty());
        CHECK(book.save("id-sam2", "Sam", "Vers=100\nStyle=\"stomp\"\nName=\"Sam\"\n", 3000).empty());
        REQUIRE(book.guests().size() == 3);
        CHECK(book.guests()[0].id == "id-sam2");  // newest first
        CHECK(book.guests()[0].label == "Sam 2");  // a second Sam: numbered
        CHECK(book.guests()[1].label == "Sam");
        CHECK(std::filesystem::exists(dir / "Robin.txt"));
        CHECK(std::filesystem::exists(dir / "Sam 2.txt"));
        // The file: their scene text, plus who they are and when they were last seen.
        const auto text = book.sceneText("id-robin");
        REQUIRE(text);
        CHECK(text->find("Style=\"jazz\"") != std::string::npos);
        CHECK(text->find("Guest=\"id-robin\"") != std::string::npos);
        CHECK(text->find("Seen=\"1000\"") != std::string::npos);
        // Checking in again with a new look: the same file, updated.
        CHECK(book.save("id-robin", "Robin", "Vers=100\nStyle=\"disco\"\nName=\"Robin\"\n", 5000).empty());
        CHECK(book.guests().size() == 3);
        CHECK(book.find("id-robin")->style == "disco");
        CHECK(book.guests()[0].id == "id-robin");
        CHECK(book.sceneText("id-robin")->find("Guest=") == book.sceneText("id-robin")->rfind("Guest="));  // once
        // A new name: the file follows it.
        CHECK(book.save("id-sam1", "Samantha", "Vers=100\nStyle=\"house\"\nName=\"Samantha\"\n", 6000).empty());
        CHECK(book.find("id-sam1")->label == "Samantha");
        CHECK_FALSE(std::filesystem::exists(dir / "Sam.txt"));
        CHECK(std::filesystem::exists(dir / "Samantha.txt"));
        // Found by id or by its label.
        CHECK(book.find("Sam 2")->id == "id-sam2");
        CHECK(book.find("nobody") == nullptr);
    }
    // Read back from the folder (a restart).
    GuestBook again(dir);
    REQUIRE(again.guests().size() == 3);
    CHECK(again.find("id-robin")->seen == 5000);
    CHECK(again.find("id-sam2")->label == "Sam 2");
    CHECK(again.find("id-sam1")->name == "Samantha");
    std::filesystem::remove_all(dir);
}

TEST_CASE("guest book: file names from guests' names") {
    using reforce::web::guestFileStem;
    CHECK(guestFileStem("Robin") == "Robin");
    CHECK(guestFileStem("../../etc/passwd") == "etcpasswd");
    CHECK(guestFileStem("  ") == "Guest");
    CHECK(guestFileStem("Zoë ✨") == "Zoë ✨");
    CHECK(guestFileStem(std::string(60, 'a')).size() == 40);
    CHECK(guestFileStem(std::string(39, 'a') + "ë") == std::string(39, 'a'));  // cut before a character, not in it
}

TEST_CASE("guest book: past guests imported from the service's guests") {
    using reforce::web::GuestBook;
    const auto dir = freshDir("guestimport");
    GuestBook book(dir);
    const char* dump = R"([
        {"id":"g1","name":"Robin","dancer":"{\"Style\":\"chill\",\"HairStyle\":\"bob\"}","at":1700000000000},
        {"id":"g2","name":"Ray","dancer":{"Style":"pop"},"at":null},
        {"id":"g3","name":"Broken"}])";
    std::string err;
    CHECK(reforce::web::importGuests(dump, book, &err) == 2);
    REQUIRE(book.guests().size() == 2);
    CHECK(book.find("g1")->style == "chill");
    CHECK(book.find("g1")->seen == 1700000000000);
    const auto text = book.sceneText("g1");
    REQUIRE(text);
    CHECK(text->find("HairStyle=\"bob\"") != std::string::npos);
    CHECK(text->find("Name=\"Robin\"") != std::string::npos);
    CHECK(reforce::web::importGuests("not json", book, &err) == -1);
    std::filesystem::remove_all(dir);
}

TEST_CASE("guests: the read key goes out as a bearer token; a refused key says so") {
    httplib::Server srv;
    std::atomic<int> polls{0};
    std::string auth;
    std::mutex authMu;
    srv.Get("/api/checkins", [&](const httplib::Request& req, httplib::Response& res) {
        ++polls;
        {
            std::lock_guard<std::mutex> lock(authMu);
            auth = req.get_header_value("Authorization");
        }
        if (req.get_header_value("Authorization") != "Bearer s3cret") {
            res.status = 401;
            return;
        }
        res.set_content(R"({"seq":0,"checkins":[]})", "application/json");
    });
    const int port = srv.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::thread th([&] { srv.listen_after_bind(); });
    const std::string url = "http://127.0.0.1:" + std::to_string(port);

    reforce::web::GuestFeed feed;
    feed.configure(true, url, 1.0, "s3cret");
    for (int i = 0; i < 100 && feed.status().empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(feed.status() == "ok");
    {
        std::lock_guard<std::mutex> lock(authMu);
        CHECK(auth == "Bearer s3cret");
    }
    feed.configure(true, url, 1.0, "wrong");
    for (int i = 0; i < 150 && feed.status() == "ok"; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(feed.status() == "HTTP 401: wrong reforce.Guests.Key");
    feed.configure(false, "");
    srv.stop();
    th.join();
}

TEST_CASE("guests: the floor goes out with each poll; social settings come back") {
    httplib::Server srv;
    std::mutex mu;
    std::string lastFloor, lastBust;
    bool hadBust = false;
    srv.Get("/api/checkins", [&](const httplib::Request& req, httplib::Response& res) {
        {
            std::lock_guard<std::mutex> lock(mu);
            lastFloor = req.get_param_value("floor");
            lastBust = req.get_param_value("bust");
            hadBust = req.has_param("bust");
        }
        res.set_content(R"({"seq":0,"checkins":[],"social":[{"id":"g1","vibe":"solo","with":""},)"
                        R"({"id":"g2","vibe":"crew","with":"g1"},{"vibe":"crew"}]})",
                        "application/json");
    });
    const int port = srv.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::thread th([&] { srv.listen_after_bind(); });

    reforce::web::GuestFeed feed;
    feed.setFloor({"g1", "g2", "a b"});
    feed.configure(true, "http://127.0.0.1:" + std::to_string(port), 1.0);
    reforce::web::GuestFeed::Batch b;
    for (int i = 0; i < 100 && !b.hasSocial; ++i) {
        b = feed.take();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(b.hasSocial);
    REQUIRE(b.social.size() == 2);  // the one without an id is skipped
    CHECK(b.social[0].id == "g1");
    CHECK(b.social[0].vibe == "solo");
    CHECK(b.social[1].with == "g1");
    {
        std::lock_guard<std::mutex> lock(mu);
        CHECK(lastFloor == "g1,g2,a b");  // (sent as a%20b)
        CHECK(hadBust);  // (sent, empty: this reforce reports busts, nobody has one)
        CHECK(lastBust == "");
    }
    CHECK(!feed.take().hasSocial);  // handed over once per poll
    feed.configure(false, "");
    srv.stop();
    th.join();

    // A service without social settings: none handed over.
    const auto p = reforce::web::parseGuestPoll(R"({"seq":1,"checkins":[]})");
    REQUIRE(p);
    CHECK(!p->hasSocial);
}

TEST_CASE("guests: requests (Bust a move) come once each, after reqAfter; a service without them sends none") {
    // docs/guests.md "requests" (Peter, 2026-11-27).
    httplib::Server srv;
    std::mutex mu;
    std::vector<std::string> reqAfter;
    srv.Get("/api/checkins", [&](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(mu);
        reqAfter.push_back(req.get_param_value("reqAfter"));
        const bool first = reqAfter.size() == 1;
        res.set_content(first ? R"({"seq":0,"checkins":[],"reqSeq":4,"requests":[{"seq":3,"id":"g1","kind":"bust"},)"
                                R"({"seq":4,"id":"g2","kind":"somethingnew"},{"seq":5,"kind":"bust"}]})"
                              : R"({"seq":0,"checkins":[],"reqSeq":4,"requests":[]})",
                        "application/json");
    });
    const int port = srv.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::thread th([&] { srv.listen_after_bind(); });
    reforce::web::GuestFeed feed;
    feed.configure(true, "http://127.0.0.1:" + std::to_string(port), 1.0);
    std::vector<reforce::web::GuestRequest> got;
    for (int i = 0; i < 150; ++i) {
        auto b = feed.take();
        got.insert(got.end(), b.requests.begin(), b.requests.end());
        {
            std::lock_guard<std::mutex> lock(mu);
            if (reqAfter.size() >= 2) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    auto b = feed.take();
    got.insert(got.end(), b.requests.begin(), b.requests.end());
    feed.configure(false, "");
    srv.stop();
    th.join();
    REQUIRE(got.size() == 2);  // (the one without an id is skipped; an unknown kind is handed over, the stage ignores it)
    CHECK(got[0].id == "g1");
    CHECK(got[0].kind == "bust");
    CHECK(got[1].kind == "somethingnew");
    REQUIRE(reqAfter.size() >= 2);
    CHECK(reqAfter[0] == "-1");
    CHECK(reqAfter[1] == "4");
    // An older service: no requests, nothing happens.
    const auto p = reforce::web::parseGuestPoll(R"({"seq":1,"checkins":[]})");
    REQUIRE(p);
    CHECK(p->requests.empty());
    CHECK(p->reqLatest == -1);
}

TEST_CASE("guests: each guest's bust goes out with the polls (docs/guests.md \"bust\")") {
    using reforce::web::GuestBust;
    CHECK(reforce::web::bustParam({}) == "");
    CHECK(reforce::web::bustParam({{"g1", GuestBust::State::Waiting, 12.4}, {"g2", GuestBust::State::Room, 20.6},
                                   {"g3", GuestBust::State::On, 57}, {"a b", GuestBust::State::Waiting, -1}}) ==
          "g1:wait:12,g2:room:21,g3:on:57,a%20b:wait");
    httplib::Server srv;
    std::mutex mu;
    std::vector<std::string> sent;
    srv.Get("/api/checkins", [&](const httplib::Request& req, httplib::Response& res) {
        {
            std::lock_guard<std::mutex> lock(mu);
            sent.push_back(req.get_param_value("bust"));
        }
        res.set_content(R"({"seq":0,"checkins":[]})", "application/json");
    });
    const int port = srv.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::thread th([&] { srv.listen_after_bind(); });
    reforce::web::GuestFeed feed;
    feed.setBusts({{"g1", GuestBust::State::On, 30}});
    feed.configure(true, "http://127.0.0.1:" + std::to_string(port), 1.0);
    for (int i = 0; i < 150; ++i) {
        {
            std::lock_guard<std::mutex> lock(mu);
            if (!sent.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    feed.configure(false, "");
    srv.stop();
    th.join();
    REQUIRE(!sent.empty());
    CHECK(sent[0] == "g1:on:30");
}
