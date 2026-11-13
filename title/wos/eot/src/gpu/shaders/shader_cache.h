// gpu/shaders/shader_cache.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <cstddef>
#include <cstdint>

struct ShaderCacheEntry {
  uint64_t hash;
  uint32_t dxilOffset;
  uint32_t dxilSize;
  uint32_t spirvOffset;
  uint32_t spirvSize;
  uint32_t specConstantsMask;
  uint32_t vertexLayoutOffset;
  uint32_t vertexLayoutCount;
  uint32_t vfetchCodeOffset;
  uint32_t usesFloatConstants;
  uint32_t interpolantMask;
  uint32_t fullDxilOffset;
  uint32_t fullDxilSize;
  uint32_t fullSpirvOffset;
  uint32_t fullSpirvSize;
  uint32_t posDxilOffset;
  uint32_t posDxilSize;
  uint32_t posSpirvOffset;
  uint32_t posSpirvSize;
  uint32_t velDxilOffset;
  uint32_t velDxilSize;
  uint32_t velSpirvOffset;
  uint32_t velSpirvSize;
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const uint32_t g_shaderVertexLayouts[];
extern const size_t g_shaderCacheEntryCount;

extern const uint8_t g_compressedDxilCache[];
extern const size_t g_dxilCacheCompressedSize;
extern const size_t g_dxilCacheDecompressedSize;

extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
