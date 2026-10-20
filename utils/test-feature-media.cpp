// clang++ -std=c++14 -Isrc -Ilib/ncutil/src -Ilib/common/src utils/test-feature-media.cpp -o /tmp/nchat-feature-media-test && /tmp/nchat-feature-media-test
#include "featuremedia.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <sys/time.h>

int main()
{
  using FeatureMedia::ParseResult;
  const FeatureMedia::Params params{{"chatId", "test-chat"}, {"chatName", "Test chat"}};
  assert(!ParseResult("not JSON", true, params).success);
  assert(!ParseResult("[]", false, params).success);
  assert(!ParseResult(R"({"text":" \n\t"})", false, params).success);
  assert(!ParseResult(R"({"error":"Permission denied"})", true, params).success);
  const auto cancelled = ParseResult(R"({"cancelled":true})", true, params);
  assert(cancelled.success && cancelled.title == "Cancelled" && cancelled.items.empty());
  const auto transcript = ParseResult(R"({"text":"A complete\nUnicode transcript 🎤","language":"en_GB"})", false, params);
  assert(transcript.success && transcript.text == "A complete\nUnicode transcript 🎤");
  assert(transcript.items.size() == 1 && transcript.items[0].operation == "local.copy");
  assert(transcript.items[0].params.at("text") == transcript.text);
  char temporary[] = "/tmp/nchat-feature-media.XXXXXX";
  assert(mkdtemp(temporary));
  const std::string path = std::string(temporary) + "/audio.ogg";
  { std::ofstream file(path); file << "synthetic fixture, never audio"; }
  const std::string json = "{\"path\":\"" + path + "\",\"mediaKind\":\"voice\",\"durationSec\":1.25,\"mimeType\":\"audio/ogg\",\"waveform\":\"AAEC\"}";
  const auto recording = ParseResult(json, true, params, temporary);
  assert(recording.success && recording.items.size() == 2);
  assert(recording.items[0].operation == "local.preview");
  const auto& send = recording.items[1];
  assert(send.operation == "send_recording" && send.confirm && send.detail == "Test chat");
  assert(send.params.at("chatId") == "test-chat" && send.params.at("waveform") == "AAEC");
  std::string videoJson = json;
  videoJson.replace(videoJson.find("voice"), 5, "video");
  const auto video = ParseResult(videoJson, true, params, temporary);
  assert(video.success && video.items[1].params.at("length") == "480");
  videoJson.insert(videoJson.size() - 1, ",\"length\":384");
  assert(ParseResult(videoJson, true, params, temporary).items[1].params.at("length") == "384");
  assert(!ParseResult(json, true, params, "/etc").success);
  assert(!ParseResult(R"({"path":"/etc/hosts","mediaKind":"voice","durationSec":0,"mimeType":"audio/ogg"})", true, params).success);
  const std::string session = FeatureMedia::PrivateDirectory(temporary);
  struct stat info{};
  assert(stat(session.c_str(), &info) == 0 && (info.st_mode & 0777) == 0700);
  const std::string resultPath = session + "/result.json";
  { std::ofstream file(resultPath); file << "{}"; }
  assert(FeatureMedia::ReadResult(resultPath) == "{}");
  assert(unlink(resultPath.c_str()) == 0 && symlink(path.c_str(), resultPath.c_str()) == 0);
  bool rejected = false;
  try { FeatureMedia::ReadResult(resultPath); } catch (const std::exception&) { rejected = true; }
  assert(rejected);
  assert(unlink(resultPath.c_str()) == 0 && rmdir(session.c_str()) == 0);
  assert(FeatureMedia::SearchTranscripts("", temporary).items.empty());
  const std::string older = FeatureMedia::PrivateDirectory(temporary);
  const std::string newer = FeatureMedia::PrivateDirectory(temporary);
  const std::string legacy = FeatureMedia::PrivateDirectory(temporary);
  auto writeTranscript = [](const std::string& directory, const std::string& text, time_t modified) {
    const std::string path = directory + "/transcript.txt";
    { std::ofstream file(path); file << text; }
    struct timeval times[2] = {{modified, 0}, {modified, 0}};
    assert(utimes(path.c_str(), times) == 0);
  };
  writeTranscript(older, "First complete birthday plan 🎂\nSecond line", 100);
  writeTranscript(newer, "NEW birthday arrangements\nNothing omitted", 300);
  writeTranscript(legacy, "Legacy transcript without context", 200);
  FeatureMedia::Params context{{"profile", "Telegram_test"}, {"chatId", "chat"}, {"msgId", "message"},
    {"chatName", "Test friend"}, {"path", "/synthetic/audio.ogg"}, {"timeSent", "12345"}, {"secret", "must not persist"}};
  FeatureMedia::SaveTranscriptContext(older, context);
  FeatureMedia::SaveTranscriptContext(newer, context);
  assert(stat((older + "/context.json").c_str(), &info) == 0 && (info.st_mode & 0777) == 0600);
  assert(FeatureMedia::ReadResult(older + "/context.json").find("must not persist") == std::string::npos);
  auto found = FeatureMedia::SearchTranscripts("BIRTHDAY", temporary);
  assert(found.success && found.items.size() == 4);
  assert(found.items[0].operation == "local.message" && found.items[1].operation == "local.preview");
  assert(found.items[0].params.at("text") == "NEW birthday arrangements\nNothing omitted");
  assert(found.items[0].params.at("profile") == "Telegram_test" && found.items[0].params.at("timeSent") == "12345");
  assert(found.items[0].params.at("sourcePath") == "/synthetic/audio.ogg" && found.items[0].params.at("fileInfo").empty());
  assert(FeatureMedia::SearchTranscripts("TEST FRIEND", temporary).items.size() == 4);
  assert(FeatureMedia::Fold("ÄPFEL") == FeatureMedia::Fold("äpfel"));
  assert(FeatureMedia::SearchTranscripts("", temporary).items.size() == 6);
  const std::string linked = std::string(temporary) + "/session.ABCDEF";
  assert(symlink(older.c_str(), linked.c_str()) == 0);
  assert(FeatureMedia::SearchTranscripts("birthday", temporary).items.size() == 4);
  assert(unlink(linked.c_str()) == 0);
  assert(unlink((legacy + "/transcript.txt").c_str()) == 0);
  assert(symlink((older + "/transcript.txt").c_str(), (legacy + "/transcript.txt").c_str()) == 0);
  assert(FeatureMedia::SearchTranscripts("birthday", temporary).items.size() == 4);
  for (const auto& directory : {older, newer, legacy})
  {
    unlink((directory + "/context.json").c_str());
    assert(unlink((directory + "/transcript.txt").c_str()) == 0 && rmdir(directory.c_str()) == 0);
  }
  assert(chmod(temporary, 0755) == 0);
  assert(!FeatureMedia::SearchTranscripts("", temporary).success);
  rejected = false;
  try { FeatureMedia::PrivateDirectory(temporary); } catch (const std::exception&) { rejected = true; }
  assert(rejected);
  assert(unlink(path.c_str()) == 0 && rmdir(temporary) == 0);
  std::cout << "Feature media parsing/archive tests passed; no native app or permission prompt was opened.\n";
}
