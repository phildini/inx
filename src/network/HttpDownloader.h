#pragma once

#include <SDCardManager.h>

#include <functional>
#include <string>

class HttpDownloader {
 public:
  using ProgressCallback = std::function<void(size_t downloaded, size_t total)>;

  enum DownloadError {
    OK = 0,
    HTTP_ERROR,
    FILE_ERROR,
    ABORTED,
  };

  static bool fetchUrl(const std::string& url, std::string& outContent);

  static bool fetchUrl(const std::string& url, Stream& stream);

  static bool fetchUrl(const std::string& url, std::string& outContent,
                       const std::string& username, const std::string& password);

  static bool fetchUrl(const std::string& url, Stream& stream,
                       const std::string& username, const std::string& password);

  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      ProgressCallback progress = nullptr);

  static DownloadError downloadToFile(const std::string& url, const std::string& destPath,
                                      const std::string& username, const std::string& password,
                                      ProgressCallback progress = nullptr);

 private:
  static constexpr size_t DOWNLOAD_CHUNK_SIZE = 1024;
};
