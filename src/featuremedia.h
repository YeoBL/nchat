#pragma once

#include "fileutil.h"
#include "protocol.h"
#include "strutil.h"
#include "uiconfig.h"
#include "../lib/ncutil/ext/cereal/include/cereal/external/rapidjson/document.h"
#include "../lib/ncutil/ext/cereal/include/cereal/external/rapidjson/stringbuffer.h"
#include "../lib/ncutil/ext/cereal/include/cereal/external/rapidjson/writer.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <codecvt>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <locale>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

// Native permission prompts and recording controls belong to the helper apps.
// Workers only consume their private results; they never access the model/UI.
namespace FeatureMedia
{
using Params = std::map<std::string, std::string>;
inline std::string Get(const Params& values, const std::string& key)
{
  const auto it = values.find(key);
  return it == values.end() ? "" : it->second;
}

inline ClientActionResult Error(const std::string& text)
{
  ClientActionResult result;
  result.success = false;
  result.title = "Media unavailable";
  result.text = text;
  return result;
}

inline ClientActionResult ParseResult(const std::string& json, bool recording,
                                     const Params& params, const std::string& directory = "")
{
  rapidjson::Document data;
  data.Parse<rapidjson::kParseValidateEncodingFlag>(json.data(), json.size());
  if (data.HasParseError() || !data.IsObject()) return Error("The media helper returned an invalid result.");
  auto value = [&](const char* key) -> std::string {
    return data.HasMember(key) && data[key].IsString() ?
      std::string(data[key].GetString(), data[key].GetStringLength()) : "";
  };
  if (data.HasMember("cancelled") && data["cancelled"].IsBool() && data["cancelled"].GetBool())
  {
    ClientActionResult result;
    result.title = "Cancelled";
    result.text = recording ? "No recording was sent." : "Transcription cancelled.";
    return result;
  }
  if (!value("error").empty()) return Error(value("error"));
  ClientActionResult result;
  if (!recording)
  {
    result.title = "Transcript";
    result.text = value("text");
    if (result.text.find_first_not_of(" \t\r\n") == std::string::npos)
      return Error("The helper did not return a transcript.");
    ClientActionItem copy;
    copy.title = "Copy transcript";
    copy.operation = "local.copy";
    copy.params["text"] = result.text;
    result.items.push_back(copy);
    return result;
  }
  const std::string path = value("path"), kind = value("mediaKind"), mime = value("mimeType");
  struct stat info{};
  char resolved[PATH_MAX], root[PATH_MAX];
  if (path.empty() || path[0] != '/' || path.find('\0') != std::string::npos ||
      !realpath(path.c_str(), resolved) || stat(resolved, &info) || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
      (!directory.empty() && (!realpath(directory.c_str(), root) ||
       std::string(resolved).find(std::string(root) + "/") != 0)) ||
      (kind != "voice" && kind != "video") || mime.empty() ||
      !data.HasMember("durationSec") || !data["durationSec"].IsNumber() ||
      !std::isfinite(data["durationSec"].GetDouble()) || data["durationSec"].GetDouble() <= 0 ||
      data["durationSec"].GetDouble() > 86400)
    return Error("The helper did not return a valid, non-empty recording.");
  result.title = "Recording ready";
  result.text = "Preview the recording, then choose Send recording when you are ready. Nothing has been sent.";
  ClientActionItem preview;
  preview.title = "Open recording";
  preview.operation = "local.preview";
  preview.params["path"] = resolved;
  result.items.push_back(preview);
  ClientActionItem send;
  send.title = "Send recording";
  send.detail = Get(params, "chatName");
  send.operation = "send_recording";
  send.params = params;
  send.params["path"] = resolved;
  send.params["mediaKind"] = kind;
  send.params["mimeType"] = mime;
  send.params["durationSec"] = std::to_string(data["durationSec"].GetDouble());
  send.params["waveform"] = value("waveform");
  if (kind == "video")
  {
    const int length = data.HasMember("length") && data["length"].IsInt() ? data["length"].GetInt() : 480;
    if (length < 1 || length > 640) return Error("The video recording has an invalid size.");
    send.params["length"] = std::to_string(length);
  }
  send.confirm = true;
  result.items.push_back(send);
  return result;
}

inline std::string PrivateDirectory(const std::string& base)
{
  if (mkdir(base.c_str(), 0700) != 0 && errno != EEXIST)
    throw std::runtime_error("Could not create a private media directory.");
  struct stat info{};
  if (lstat(base.c_str(), &info) || !S_ISDIR(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 077))
    throw std::runtime_error("The media directory must be private and owned by your account.");
  std::string path = base + "/session.XXXXXX";
  if (!mkdtemp(&path[0])) throw std::runtime_error("Could not create a private media session.");
  return path;
}

inline std::string ReadResult(const std::string& path)
{
  const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) throw std::runtime_error("The media helper closed without a result. Please try again.");
  struct stat info{};
  if (fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
      info.st_size <= 0 || info.st_size > 2 * 1024 * 1024)
  {
    close(fd);
    throw std::runtime_error("The media helper returned an invalid result file.");
  }
  std::string result(static_cast<size_t>(info.st_size), '\0');
  size_t offset = 0;
  while (offset < result.size())
  {
    const ssize_t count = read(fd, &result[offset], result.size() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    offset += static_cast<size_t>(count);
  }
  close(fd);
  if (offset != result.size()) throw std::runtime_error("Could not read the complete media result.");
  return result;
}

inline void SaveTranscriptContext(const std::string& directory, const Params& params)
{
  rapidjson::Document data(rapidjson::kObjectType);
  auto& allocator = data.GetAllocator();
  for (const auto* key : {"profile", "chatId", "msgId", "chatName", "sourcePath", "timeSent"})
  {
    std::string value = Get(params, key);
    if (std::string(key) == "sourcePath" && value.empty()) value = Get(params, "path");
    data.AddMember(rapidjson::Value(key, allocator).Move(), rapidjson::Value(value.c_str(), value.size(), allocator).Move(), allocator);
  }
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  data.Accept(writer);
  std::string temporary = directory + "/.context.XXXXXX";
  const int fd = mkstemp(&temporary[0]);
  if (fd < 0) throw std::runtime_error("Could not save the transcript's chat context.");
  size_t offset = 0;
  while (offset < buffer.GetSize())
  {
    const ssize_t count = write(fd, buffer.GetString() + offset, buffer.GetSize() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    offset += static_cast<size_t>(count);
  }
  const bool saved = offset == buffer.GetSize() && fsync(fd) == 0;
  close(fd);
  if (!saved || rename(temporary.c_str(), (directory + "/context.json").c_str()))
  {
    unlink(temporary.c_str());
    throw std::runtime_error("The transcript was saved, but its chat context could not be saved.");
  }
}

inline std::string Fold(std::string text)
{
  try
  {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;
    auto wide = converter.from_bytes(text);
    if (!wide.empty()) std::use_facet<std::ctype<wchar_t>>(std::locale("")).tolower(&wide[0], &wide[0] + wide.size());
    return converter.to_bytes(wide);
  }
  catch (...)
  {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
    return text;
  }
}

// Only this feature's direct private session directories are examined. The cap
// bounds UI output, not matching: newest matches are selected after scanning.
inline ClientActionResult SearchTranscripts(const std::string& query, const std::string& base)
{
  ClientActionResult result;
  result.title = "Saved transcripts";
  struct Match { std::string path, text, label; Params context; time_t saved; };
  std::vector<Match> matches;
  struct stat info{};
  if (lstat(base.c_str(), &info))
  {
    if (errno != ENOENT) return Error("The saved transcript archive could not be opened.");
  }
  else
  {
    if (!S_ISDIR(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 077))
      return Error("The transcript archive is not a private directory owned by your account.");
    DIR* directory = opendir(base.c_str());
    if (!directory) return Error("The saved transcript archive could not be opened.");
    const std::string needle = Fold(query);
    while (const auto* entry = readdir(directory))
    {
      const std::string name = entry->d_name;
      if (name.size() != 14 || name.compare(0, 8, "session.") != 0 ||
          name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 8) != std::string::npos) continue;
      const std::string path = base + "/" + name;
      if (lstat(path.c_str(), &info) || !S_ISDIR(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 077)) continue;
      try
      {
        Match match;
        match.path = path + "/transcript.txt";
        match.text = ReadResult(match.path);
        if (match.text.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        if (lstat(match.path.c_str(), &info) || !S_ISREG(info.st_mode)) continue;
        match.saved = info.st_mtime;
        try
        {
          const std::string metadata = ReadResult(path + "/context.json");
          rapidjson::Document data;
          data.Parse<rapidjson::kParseValidateEncodingFlag>(metadata.data(), metadata.size());
          if (!data.HasParseError() && data.IsObject())
            for (const auto* key : {"profile", "chatId", "msgId", "chatName", "sourcePath", "timeSent"})
              if (data.HasMember(key) && data[key].IsString())
                match.context[key] = std::string(data[key].GetString(), data[key].GetStringLength());
        }
        catch (...) { } // Older saved transcripts still appear without chat metadata.
        match.label = Get(match.context, "chatName");
        if (match.label.empty()) match.label = "Saved audio transcript";
        if (needle.empty() || Fold(match.text + "\n" + match.label).find(needle) != std::string::npos)
          matches.push_back(std::move(match));
      }
      catch (...) { } // Ignore unfinished, oversized, or symlinked result files.
    }
    closedir(directory);
  }
  std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
    return a.saved == b.saved ? a.path > b.path : a.saved > b.saved;
  });
  const size_t count = std::min<size_t>(100, matches.size());
  result.text = matches.empty() ? "No saved transcripts match. Transcribe an audio message to add it here." :
    std::to_string(matches.size()) + " matching transcript(s). Nothing is uploaded or marked read.";
  if (matches.size() > count) result.text += " Showing the newest 100; narrow your search for older matches.";
  for (size_t index = 0; index < count; ++index)
  {
    const auto& match = matches[index];
    ClientActionItem item;
    item.title = match.label + " · Transcript";
    item.detail = match.text;
    item.operation = "local.message";
    item.params = match.context;
    for (const auto* key : {"profile", "chatId", "msgId", "timeSent"})
      if (!item.params.count(key)) item.params[key] = ""; // Do not inherit an unrelated open chat for legacy files.
    item.params["text"] = match.text;
    item.params["path"] = match.path;
    item.params["fileInfo"] = "";
    result.items.push_back(item);
    item.title = "Open saved transcript · " + match.label;
    item.detail.clear();
    item.operation = "local.preview";
    result.items.push_back(item);
  }
  return result;
}

inline ClientActionResult SearchTranscripts(const std::string& query)
{ return SearchTranscripts(query, FileUtil::GetApplicationDir() + "/transcripts"); }

inline void Run(bool recording, bool video, const Params& params, std::function<void(ClientActionResult)> done,
                const std::string& preparedDirectory = "")
{
  const std::string name = recording ? "nchat-record.app" : "nchat-transcribe.app";
  std::string app = UiConfig::GetStr(recording ? "record_app" : "transcribe_app");
  if (app.empty()) app = FileUtil::GetApplicationDir() + "/helpers/" + name;
  const std::string base = FileUtil::GetApplicationDir() + (recording ? "/recordings" : "/transcripts");
  std::thread([recording, video, params, done, app, base, preparedDirectory]() {
    ClientActionResult result;
    std::string directory = preparedDirectory;
    try
    {
      if (access(app.c_str(), R_OK)) throw std::runtime_error("The native media helper is not installed. Rebuild/install nchat's helper apps first.");
      if (!recording && Get(params, "path").empty()) throw std::runtime_error("Select a downloaded audio message first.");
      if (directory.empty()) directory = PrivateDirectory(base);
      auto quote = [](const std::string& text) { return "'" + StrUtil::EscapeSingleQuote(text) + "'"; };
      std::string command = "/usr/bin/open -W -n " + quote(app) + " --args --result " + quote(directory + "/result.json");
      command += recording ? std::string(video ? " --video" : " --voice") + " --output-dir " + quote(directory) :
        " --output " + quote(directory + "/transcript.txt") + " " + quote(Get(params, "path"));
      const int status = std::system((command + " >/dev/null 2>&1").c_str());
      // LaunchServices can stop waiting before the app exits. Only its atomic JSON
      // result establishes success; opening the helper does not establish it.
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(status == 0 ? 900 : 5);
      while (access((directory + "/result.json").c_str(), F_OK) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      result = ParseResult(ReadResult(directory + "/result.json"), recording, params, directory);
      if (!recording && result.success && !result.items.empty())
      {
        SaveTranscriptContext(directory, params);
        ClientActionItem saved;
        saved.title = "Open saved transcript";
        saved.operation = "local.preview";
        saved.params["path"] = directory + "/transcript.txt";
        result.items.push_back(saved);
      }
    }
    catch (const std::exception& error) { result = Error(error.what()); }
    if (!directory.empty() && preparedDirectory.empty())
    {
      unlink((directory + "/result.json").c_str());
      rmdir(directory.c_str()); // Only empty sessions; never delete recordings/transcripts.
    }
    if (done) done(std::move(result));
  }).detach();
}

inline void Record(bool video, const Params& params, std::function<void(ClientActionResult)> done)
{ Run(true, video, params, std::move(done)); }
inline void Transcribe(const Params& params, std::function<void(ClientActionResult)> done)
{ Run(false, false, params, std::move(done)); }

inline bool IsSessionName(const std::string& name)
{
  return name.size() == 14 && name.compare(0, 8, "session.") == 0 &&
    name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 8) == std::string::npos;
}

inline void SaveJobStatus(const std::string& directory, const ClientActionResult& result, bool finished)
{
  rapidjson::Document data(rapidjson::kObjectType);
  auto& allocator = data.GetAllocator();
  data.AddMember("finished", finished, allocator);
  data.AddMember("success", result.success, allocator);
  data.AddMember("title", rapidjson::Value(result.title.c_str(), result.title.size(), allocator).Move(), allocator);
  data.AddMember("text", rapidjson::Value(result.text.c_str(), result.text.size(), allocator).Move(), allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  data.Accept(writer);
  std::string path = directory + "/.status.XXXXXX";
  const int fd = mkstemp(&path[0]);
  if (fd < 0) throw std::runtime_error("Could not save transcription status.");
  size_t offset = 0;
  while (offset < buffer.GetSize())
  {
    const ssize_t count = write(fd, buffer.GetString() + offset, buffer.GetSize() - offset);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    offset += static_cast<size_t>(count);
  }
  const bool saved = offset == buffer.GetSize() && fsync(fd) == 0;
  close(fd);
  if (!saved || rename(path.c_str(), (directory + "/status.json").c_str()))
  {
    unlink(path.c_str());
    throw std::runtime_error("Could not save transcription status.");
  }
}

inline ClientActionResult TranscriptionStatus(const std::string& base, const std::string& session = "")
{
  ClientActionResult result;
  result.title = "Transcription status";
  result.text = "Transcription runs separately: you can close tools and keep chatting. Refresh here for its outcome.";
  struct stat info{};
  if (lstat(base.c_str(), &info))
  {
    if (errno != ENOENT) return Error("Transcription status could not be opened.");
    result.text = "No background transcriptions yet.";
    return result;
  }
  if (!S_ISDIR(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 077))
    return Error("The transcript archive must be private and owned by your account.");
  if (!session.empty() && !IsSessionName(session)) return Error("Invalid transcription job.");
  std::vector<std::pair<time_t, std::string>> jobs;
  DIR* directory = opendir(base.c_str());
  if (!directory) return Error("Transcription status could not be opened.");
  while (const auto* entry = readdir(directory))
  {
    const std::string name = entry->d_name;
    if (!IsSessionName(name) || (!session.empty() && session != name)) continue;
    if (lstat((base + "/" + name).c_str(), &info) || !S_ISDIR(info.st_mode) ||
        info.st_uid != getuid() || (info.st_mode & 077)) continue;
    if (lstat((base + "/" + name + "/status.json").c_str(), &info) || !S_ISREG(info.st_mode)) continue;
    jobs.emplace_back(info.st_mtime, name);
  }
  closedir(directory);
  std::sort(jobs.rbegin(), jobs.rend());
  if (jobs.empty()) result.text = session.empty() ? "No background transcriptions yet." : "This transcription job is no longer available.";
  for (size_t index = 0; index < jobs.size(); ++index)
  {
    const auto path = base + "/" + jobs[index].second;
    try
    {
      const auto json = ReadResult(path + "/status.json");
      rapidjson::Document data;
      data.Parse<rapidjson::kParseValidateEncodingFlag>(json.data(), json.size());
      if (data.HasParseError() || !data.IsObject() || !data.HasMember("finished") ||
          !data["finished"].IsBool() || !data.HasMember("success") || !data["success"].IsBool() ||
          !data.HasMember("text") || !data["text"].IsString()) continue;
      bool finished = data["finished"].GetBool(), success = data["success"].GetBool();
      std::string text(data["text"].GetString(), data["text"].GetStringLength());
      bool cancelled = data.HasMember("title") && data["title"].IsString() &&
        std::string(data["title"].GetString(), data["title"].GetStringLength()) == "Cancelled";
      // A helper may finish after nchat quits. Its atomic result remains readable
      // even when the old process could not write a final status update.
      if (!finished)
      {
        try
        {
          const auto completed = ParseResult(ReadResult(path + "/result.json"), false, {});
          finished = true;
          success = completed.success;
          text = completed.text;
          cancelled = completed.title == "Cancelled";
        }
        catch (...) { }
      }
      std::string name = "Audio transcript";
      try
      {
        const auto metadata = ReadResult(path + "/context.json");
        rapidjson::Document context;
        context.Parse<rapidjson::kParseValidateEncodingFlag>(metadata.data(), metadata.size());
        if (!context.HasParseError() && context.IsObject() && context.HasMember("chatName") && context["chatName"].IsString())
          name.assign(context["chatName"].GetString(), context["chatName"].GetStringLength());
      }
      catch (...) { }
      if (session.empty())
      {
        ClientActionItem item;
        item.title = name + (finished ? (cancelled ? " · Cancelled" : success ? " · Finished" : " · Failed") : " · Working / outcome not yet confirmed");
        item.detail = text;
        item.operation = "local.transcription.status";
        item.params["job"] = jobs[index].second;
        result.items.push_back(std::move(item));
      }
      else
      {
        result.title = name + (finished ? (cancelled ? " · Cancelled" : success ? " · Finished" : " · Failed") : " · Working");
        result.text = text;
        if (!finished) result.text += "\n\nIf nchat or the helper was stopped, the outcome may be unknown. You can retry the original message.";
        ClientActionItem copy;
        copy.title = "Copy outcome";
        copy.operation = "local.copy";
        copy.params["text"] = text;
        result.items.push_back(std::move(copy));
      }
    }
    catch (...) { } // Ignore incomplete or unsafe status files; never browse other files.
  }
  ClientActionItem refresh;
  refresh.title = "Refresh transcription status";
  refresh.operation = "local.transcription.status";
  refresh.params = {{"job", session}, {"_replacePage", "1"}};
  result.items.push_back(std::move(refresh));
  ClientActionItem archive;
  archive.title = "View saved transcripts";
  archive.operation = "local.transcripts";
  result.items.push_back(std::move(archive));
  return result;
}

inline ClientActionResult StartTranscription(const Params& params, const std::string& base,
  const std::function<void(const Params&, std::function<void(ClientActionResult)>, const std::string&)>& launch)
{
  if (Get(params, "path").empty()) return Error("Select a downloaded audio message first.");
  std::string directory;
  try
  {
    directory = PrivateDirectory(base);
    SaveTranscriptContext(directory, params);
    ClientActionResult pending;
    pending.title = "Transcription started";
    pending.text = "Working on this Mac. Close tools to continue chatting; check Transcription status in F2 for the result or any error.";
    SaveJobStatus(directory, pending, false);
    launch(params, [directory](ClientActionResult result) {
      try { SaveJobStatus(directory, result, true); }
      catch (...) { } // The helper's retained atomic result remains a fallback.
    }, directory);
    ClientActionItem status;
    status.title = "Check transcription status";
    status.operation = "local.transcription.status";
    status.params["job"] = directory.substr(directory.find_last_of('/') + 1);
    pending.items.push_back(std::move(status));
    ClientActionItem archive;
    archive.title = "View saved transcripts";
    archive.operation = "local.transcripts";
    pending.items.push_back(std::move(archive));
    return pending;
  }
  catch (const std::exception& error)
  {
    auto failed = Error(error.what());
    if (!directory.empty())
      try { SaveJobStatus(directory, failed, true); } catch (...) { }
    return failed;
  }
}

inline ClientActionResult StartTranscription(const Params& params)
{
  return StartTranscription(params, FileUtil::GetApplicationDir() + "/transcripts",
    [](const Params& values, std::function<void(ClientActionResult)> done, const std::string& directory) {
      Run(false, false, values, std::move(done), directory);
    });
}

inline ClientActionResult TranscriptionStatus(const Params& params)
{ return TranscriptionStatus(FileUtil::GetApplicationDir() + "/transcripts", Get(params, "job")); }
}
