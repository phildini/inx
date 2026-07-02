#include "ImageLevelCache.h"

#include <Arduino.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "BitmapUtil.h"
#include "GfxRenderer.h"

namespace {
constexpr uint32_t kMagic = 0x434C5849;  // IXLC
constexpr uint16_t kVersion = 1;
constexpr const char* kCacheDir = "/.system/cache";
constexpr size_t kIoBufferSize = 512;

struct CacheHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t headerSize;
  uint16_t width;
  uint16_t height;
  uint16_t rowBytes;
  uint16_t reserved;
};

uint32_t fnv1aAdd(uint32_t hash, uint8_t byte) {
  hash ^= byte;
  return hash * 16777619u;
}

uint32_t fnv1aAddUint32(uint32_t hash, uint32_t value) {
  hash = fnv1aAdd(hash, static_cast<uint8_t>(value & 0xFF));
  hash = fnv1aAdd(hash, static_cast<uint8_t>((value >> 8) & 0xFF));
  hash = fnv1aAdd(hash, static_cast<uint8_t>((value >> 16) & 0xFF));
  return fnv1aAdd(hash, static_cast<uint8_t>((value >> 24) & 0xFF));
}

uint32_t sourceSize(const std::string& path) {
  FsFile file;
  if (!SdMan.openFileForRead("ILC", path, file)) {
    return 0;
  }
  const uint32_t size = static_cast<uint32_t>(file.size());
  file.close();
  return size;
}

bool ensureCacheDir() {
  if (SdMan.exists(kCacheDir)) {
    FsFile file = SdMan.open(kCacheDir);
    const bool ok = file && file.isDirectory();
    file.close();
    if (ok) return true;
  }
  return SdMan.mkdir(kCacheDir) || SdMan.exists(kCacheDir);
}

bool shouldDrawLevel(uint8_t level, GfxRenderer::RenderMode mode, bool deviceIsX3, bool& state) {
  level &= 0x03;
  if (mode == GfxRenderer::BW) {
    state = true;
    return level > 0;
  }

  static constexpr uint8_t grayscaleCodeForLevel[4] = {0b00, 0b10, 0b11, 0b01};
  static constexpr uint8_t x3GrayscaleCodeForLevel[4] = {0b00, 0b11, 0b10, 0b01};
  const uint8_t grayscaleCode = deviceIsX3 ? x3GrayscaleCodeForLevel[level] : grayscaleCodeForLevel[level];

  if (mode == GfxRenderer::GRAYSCALE_MSB) {
    state = false;
    return (grayscaleCode & 0b10) != 0;
  }
  if (mode == GfxRenderer::GRAYSCALE_LSB) {
    state = false;
    return (grayscaleCode & 0b01) != 0;
  }
  if (mode == GfxRenderer::GRAY2_LSB) {
    state = true;
    return (mapQualityGray2Level(level, deviceIsX3) & 0b01) == 0;
  }
  if (mode == GfxRenderer::GRAY2_MSB) {
    state = true;
    return (mapQualityGray2Level(level, deviceIsX3) & 0b10) == 0;
  }
  return false;
}

}  // namespace

ImageLevelCacheWriter::~ImageLevelCacheWriter() {
  if (file_.isOpen()) {
    abort();
  }
  free(row_);
}

bool ImageLevelCacheWriter::begin(const std::string& path, const int width, const int height) {
  if (width <= 0 || height <= 0 || !ensureCacheDir()) {
    return false;
  }
  width_ = width;
  height_ = height;
  rowBytes_ = (width_ + 3) / 4;
  row_ = static_cast<uint8_t*>(malloc(rowBytes_));
  if (!row_) {
    return false;
  }
  memset(row_, 0, rowBytes_);

  if (!SdMan.openFileForWrite("ILC", path, file_)) {
    free(row_);
    row_ = nullptr;
    return false;
  }
  path_ = path;
  const CacheHeader header = {.magic = kMagic,
                              .version = kVersion,
                              .headerSize = sizeof(CacheHeader),
                              .width = static_cast<uint16_t>(width_),
                              .height = static_cast<uint16_t>(height_),
                              .rowBytes = static_cast<uint16_t>(rowBytes_),
                              .reserved = 0};
  if (file_.write(&header, sizeof(header)) != sizeof(header)) {
    abort();
    return false;
  }
  ok_ = true;
  return true;
}

void ImageLevelCacheWriter::beginRow(const int row) {
  if (!ok_ || row < 0 || row >= height_) {
    currentRow_ = -1;
    return;
  }
  while (writtenRows_ < row) {
    memset(row_, 0, rowBytes_);
    if (file_.write(row_, rowBytes_) != static_cast<size_t>(rowBytes_)) {
      abort();
      return;
    }
    writtenRows_++;
  }
  currentRow_ = row;
  memset(row_, 0, rowBytes_);
}

void ImageLevelCacheWriter::writeLevel(const int x, const uint8_t level) {
  if (!ok_ || currentRow_ < 0 || x < 0 || x >= width_) {
    return;
  }
  const int byteIdx = x >> 2;
  const int shift = 6 - ((x & 3) * 2);
  row_[byteIdx] = static_cast<uint8_t>((row_[byteIdx] & ~(0x03 << shift)) | ((level & 0x03) << shift));
}

bool ImageLevelCacheWriter::endRow() {
  if (!ok_ || currentRow_ < 0) {
    return ok_;
  }
  if (currentRow_ != writtenRows_) {
    return ok_;
  }
  if (file_.write(row_, rowBytes_) != static_cast<size_t>(rowBytes_)) {
    abort();
    return false;
  }
  writtenRows_++;
  currentRow_ = -1;
  return true;
}

bool ImageLevelCacheWriter::finalize() {
  if (!ok_) {
    abort();
    return false;
  }
  while (writtenRows_ < height_) {
    memset(row_, 0, rowBytes_);
    if (file_.write(row_, rowBytes_) != static_cast<size_t>(rowBytes_)) {
      abort();
      return false;
    }
    writtenRows_++;
  }
  file_.close();
  ok_ = false;
  return true;
}

void ImageLevelCacheWriter::abort() {
  if (file_.isOpen()) {
    file_.close();
  }
  if (!path_.empty()) {
    SdMan.remove(path_.c_str());
  }
  ok_ = false;
}

std::string ImageLevelCache::pathFor(const std::string& sourcePath, const int width, const int height,
                                     const ImageLevelCacheOptions& options) {
  uint32_t hash = 2166136261u;
  for (const char c : sourcePath) {
    hash = fnv1aAdd(hash, static_cast<uint8_t>(c));
  }
  hash = fnv1aAddUint32(hash, sourceSize(sourcePath));
  hash = fnv1aAddUint32(hash, static_cast<uint32_t>(width));
  hash = fnv1aAddUint32(hash, static_cast<uint32_t>(height));
  hash = fnv1aAdd(hash, options.cropToFill ? 1 : 0);
  hash = fnv1aAdd(hash, options.quality ? 1 : 0);
  hash = fnv1aAdd(hash, options.deviceIsX3 ? 1 : 0);

  char name[32];
  snprintf(name, sizeof(name), "/%08lx.ilc", static_cast<unsigned long>(hash));
  return std::string(kCacheDir) + name;
}

std::unique_ptr<ImageLevelCacheWriter> ImageLevelCache::createWriter(const std::string& sourcePath, const int width,
                                                                     const int height,
                                                                     const ImageLevelCacheOptions& options) {
  std::unique_ptr<ImageLevelCacheWriter> writer(new (std::nothrow) ImageLevelCacheWriter());
  if (!writer) {
    return nullptr;
  }
  if (!writer->begin(pathFor(sourcePath, width, height, options), width, height)) {
    return nullptr;
  }
  return writer;
}

bool ImageLevelCache::renderIfAvailable(GfxRenderer& renderer, const std::string& sourcePath, const int x,
                                        const int y, const int width, const int height,
                                        const ImageLevelCacheOptions& options) {
  if (width <= 0 || height <= 0) {
    return false;
  }
  const std::string path = pathFor(sourcePath, width, height, options);
  if (!SdMan.exists(path.c_str())) {
    return false;
  }

  FsFile file;
  if (!SdMan.openFileForRead("ILC", path, file)) {
    return false;
  }

  CacheHeader header;
  const bool headerOk = file.read(&header, sizeof(header)) == sizeof(header) && header.magic == kMagic &&
                        header.version == kVersion && header.headerSize == sizeof(CacheHeader) &&
                        header.width == width && header.height == height &&
                        header.rowBytes == static_cast<uint16_t>((width + 3) / 4);
  if (!headerOk) {
    file.close();
    return false;
  }

  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int visibleX0 = std::max(0, x);
  const int visibleY0 = std::max(0, y);
  const int visibleX1 = std::min(screenW, x + width);
  const int visibleY1 = std::min(screenH, y + height);
  if (visibleX1 <= visibleX0 || visibleY1 <= visibleY0) {
    file.close();
    return true;
  }

  const int rowBytes = header.rowBytes;
  uint8_t sourceRows[kIoBufferSize];
  uint8_t drawRow[kIoBufferSize];
  if (rowBytes > static_cast<int>(sizeof(sourceRows))) {
    file.close();
    return false;
  }

  const int firstSourceRow = visibleY0 - y;
  const int rowCount = visibleY1 - visibleY0;
  const size_t dataOffset = sizeof(CacheHeader) + static_cast<size_t>(firstSourceRow) * rowBytes;
  if (!file.seek(dataOffset)) {
    file.close();
    return false;
  }

  const GfxRenderer::RenderMode mode = renderer.getRenderMode();
  const bool deviceIsX3 = renderer.deviceIsX3();
  for (int row = 0; row < rowCount; row++) {
    if (file.read(sourceRows, rowBytes) != rowBytes) {
      file.close();
      return false;
    }

    const int sourceY = firstSourceRow + row;
    const int sourceX0 = visibleX0 - x;
    const int visibleWidth = visibleX1 - visibleX0;
    const int drawBytes = (visibleWidth + 7) / 8;
    memset(drawRow, 0, drawBytes);

    bool rowState = true;
    bool rowHasPixels = false;
    for (int col = 0; col < visibleWidth; col++) {
      const int sourceX = sourceX0 + col;
      const int byteIdx = sourceX >> 2;
      const int shift = 6 - ((sourceX & 3) * 2);
      const uint8_t level = (sourceRows[byteIdx] >> shift) & 0x03;

      bool state = true;
      if (shouldDrawLevel(level, mode, deviceIsX3, state)) {
        rowState = state;
        rowHasPixels = true;
        drawRow[col / 8] |= static_cast<uint8_t>(0x80 >> (col % 8));
      }
    }
    if (rowHasPixels) {
      renderer.drawPackedRow1bppInkOnly(visibleX0, y + sourceY, visibleWidth, drawRow, rowState);
    }
  }

  file.close();
  return true;
}
