#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <SDCardManager.h>

class GfxRenderer;

struct ImageLevelCacheOptions {
  bool cropToFill = false;
  bool quality = false;
  bool deviceIsX3 = false;
};

class ImageLevelCacheWriter {
 public:
  ImageLevelCacheWriter() = default;
  ImageLevelCacheWriter(const ImageLevelCacheWriter&) = delete;
  ImageLevelCacheWriter& operator=(const ImageLevelCacheWriter&) = delete;
  ~ImageLevelCacheWriter();

  bool begin(const std::string& path, int width, int height);
  void beginRow(int row);
  void writeLevel(int x, uint8_t level);
  bool endRow();
  bool finalize();
  void abort();

 private:
  FsFile file_;
  std::string path_;
  uint8_t* row_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int rowBytes_ = 0;
  int currentRow_ = -1;
  int writtenRows_ = 0;
  bool ok_ = false;
};

class ImageLevelCache {
 public:
  static bool renderIfAvailable(GfxRenderer& renderer, const std::string& sourcePath, int x, int y, int width,
                                int height, const ImageLevelCacheOptions& options);
  static std::unique_ptr<ImageLevelCacheWriter> createWriter(const std::string& sourcePath, int width, int height,
                                                             const ImageLevelCacheOptions& options);

 private:
  static std::string pathFor(const std::string& sourcePath, int width, int height,
                             const ImageLevelCacheOptions& options);
};
