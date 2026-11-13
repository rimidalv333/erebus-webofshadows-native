// gpu/pipeline/pso_predictor.h
// Copyright Rien Gupta <rgupta9@scu.edu>
// BSD 3-Clause

#pragma once

#include <string>

#include <rex/types.h>

namespace eot::gpu {

u32 PredictModelLoad(u32 model_va);

u32 PredictMaterialLoad(u32 material_va);

void PredictorNoteShaderBundle(u32 bundle_va);

void PredictorNoteShadowBias(float offset, float slope);

struct PsoPredictorStats {
  u32 models = 0, materials = 0, slots = 0, queued = 0, noTemplate = 0, shadowBiases = 0;
};
PsoPredictorStats PsoPredictorGetStats();

std::string PsoPredictorDescribeObject(u32 shader_object_va);

}
