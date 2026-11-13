// gpu/pipeline/pso_records.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <rex/types.h>

#include "gpu/d3d.h"
#include "gpu/pipeline/pipeline_cache.h"

namespace eot::gpu {

constexpr u32 kPsoCsvVersion = 4;

#if defined(EOT_MVK)
constexpr u32 kPsoHoldMaxMs = 60000;
#else
constexpr u32 kPsoHoldMaxMs = 8000;
#endif
constexpr u32 kPsoKnownPackageRows = 32;
constexpr u32 kPsoMaxThreads = 8;
constexpr u32 kPsoMinThreads = 1;

constexpr u16 kNoTemplate = 0xFFFF;
constexpr u32 kMaxRecordPackages = 8;

struct PsoRecord {
  PipelineState state;
  u32 declCount = 0;
  u8 declRaw[32 * sizeof(DeclElement)] = {};
  u64 frame = 0;
  u16 templateIndex = kNoTemplate;
  u16 packages[kMaxRecordPackages] = {};
  u32 packageCount = 0;
};

struct PsoCsvLayout {
  static constexpr u32 kColumns = 35;
  i8 index[kColumns];
  u32 fieldCount = 0;
  u32 version = kPsoCsvVersion;
};

std::string PsoCsvHeader();
std::string PsoRecordToCsv(const PsoRecord &r, std::string_view session);
bool PsoCsvParseHeader(std::string_view line, PsoCsvLayout *out);
bool PsoRecordFromCsv(const PsoCsvLayout &layout, std::string_view line, PsoRecord *out);

const std::vector<PsoRecord> &CompiledInPipelines();

enum class PsoBiasKind : u8 { None = 0, Material = 1, ShadowCamera = 2 };
struct PsoTemplate {
  u8 technique = 0, pass = 0;
  PsoBiasKind biasKind = PsoBiasKind::None;
  u32 materialClass = 0;
  PipelineState state;
};
const std::vector<PsoTemplate> &CompiledInTemplates();

std::string PsoSessionStamp();
std::string PsoSessionTag();
std::string PsoDir();
size_t LoadPsoCsvDir(const std::string &dir, std::vector<PsoRecord> &out);

void PsoCaptureConfigure();
void PsoCaptureAdd(const PsoRecord &r);
void PsoCaptureFlush(bool force, u64 guest_frame);
void PsoWriteSessionFile(const std::string &name, const std::string &header,
                         const std::vector<std::string> &rows);

}
