/**************************************************************************/
/*  rendering_shader_container_vulkan.cpp                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "rendering_shader_container_vulkan.h"

#include <thirdparty/misc/smolv.h>

// RenderingShaderContainerVulkan

const uint32_t RenderingShaderContainerVulkan::FORMAT_VERSION = 1;

uint32_t RenderingShaderContainerVulkan::_format() const {
	return 0x43565053;
}

uint32_t RenderingShaderContainerVulkan::_format_version() const {
	return FORMAT_VERSION;
}

// Returns true if the SPIR-V declares the Tessellation or Geometry capability. smolv's opcode support is
// vertex/fragment-oriented and it does not round-trip these stages: the decoded module still passes
// vkCreateShaderModule but the driver produces degenerate geometry (or rejects the pipeline). Such stages
// must be stored uncompressed.
static bool _spirv_requires_uncompressed(Span<uint8_t> p_spirv) {
	const uint32_t word_count = (uint32_t)(p_spirv.size() / sizeof(uint32_t));
	if (word_count < 5 || ((const uint32_t *)p_spirv.ptr())[0] != 0x07230203u) { // SPIR-V magic.
		return false;
	}
	const uint32_t *words = (const uint32_t *)p_spirv.ptr();
	uint32_t i = 5;
	while (i < word_count) {
		const uint32_t instr_word_count = words[i] >> 16;
		const uint32_t opcode = words[i] & 0xFFFFu;
		if (instr_word_count == 0) {
			break;
		}
		if (opcode == 17u /* OpCapability */ && (i + 1) < word_count) {
			const uint32_t capability = words[i + 1];
			if (capability == 2u /* Geometry */ || capability == 3u /* Tessellation */) {
				return true;
			}
		} else if (opcode == 15u /* OpEntryPoint */) {
			break; // Capabilities always precede entry points; nothing relevant follows.
		}
		i += instr_word_count;
	}
	return false;
}

bool RenderingShaderContainerVulkan::_set_code_from_spirv(const ReflectShader &p_shader) {
	const LocalVector<ReflectShaderStage> &p_spirv = p_shader.shader_stages;

	PackedByteArray code_bytes;
	shaders.resize(p_spirv.size());
	for (uint64_t i = 0; i < p_spirv.size(); i++) {
		RenderingShaderContainer::Shader &shader = shaders.ptrw()[i];
		const bool stage_needs_raw_spirv = _spirv_requires_uncompressed(p_spirv[i].spirv().reinterpret<uint8_t>());
		if (debug_info_enabled || stage_needs_raw_spirv) {
			// Store SPIR-V as is when debug info is required, or when smolv can't safely round-trip it
			// (tessellation / geometry stages).
			shader.code_compressed_bytes = p_spirv[i].spirv_data();
			shader.code_compression_flags = 0;
			shader.code_decompressed_size = 0;
		} else {
			// Encode into smolv.
			Span<uint8_t> spirv = p_spirv[i].spirv().reinterpret<uint8_t>();
			smolv::ByteArray smolv_bytes;
			bool smolv_encoded = smolv::Encode(spirv.ptr(), spirv.size(), smolv_bytes, smolv::kEncodeFlagStripDebugInfo);
			ERR_FAIL_COND_V_MSG(!smolv_encoded, false, "Failed to compress SPIR-V into smolv.");

			code_bytes.resize(smolv_bytes.size());
			memcpy(code_bytes.ptrw(), smolv_bytes.data(), code_bytes.size());

			// Compress.
			uint32_t compressed_size = 0;
			shader.code_decompressed_size = code_bytes.size();
			shader.code_compressed_bytes.resize(code_bytes.size());

			bool compressed = compress_code(code_bytes.ptr(), code_bytes.size(), shader.code_compressed_bytes.ptrw(), &compressed_size, &shader.code_compression_flags);
			ERR_FAIL_COND_V_MSG(!compressed, false, vformat("Failed to compress native code to native for SPIR-V #%d.", i));

			shader.code_compressed_bytes.resize(compressed_size);

			// Indicate it uses smolv for compression.
			shader.code_compression_flags |= COMPRESSION_FLAG_SMOLV;
		}

		shader.shader_stage = p_spirv[i].shader_stage;
	}

	return true;
}

RenderingShaderContainerVulkan::RenderingShaderContainerVulkan(bool p_debug_info_enabled) {
	debug_info_enabled = p_debug_info_enabled;
}

// RenderingShaderContainerFormatVulkan

Ref<RenderingShaderContainer> RenderingShaderContainerFormatVulkan::create_container() const {
	return memnew(RenderingShaderContainerVulkan(debug_info_enabled));
}

RenderingDeviceCommons::ShaderLanguageVersion RenderingShaderContainerFormatVulkan::get_shader_language_version() const {
	return SHADER_LANGUAGE_VULKAN_VERSION_1_1;
}

RenderingDeviceCommons::ShaderSpirvVersion RenderingShaderContainerFormatVulkan::get_shader_spirv_version() const {
	return SHADER_SPIRV_VERSION_1_4;
}

void RenderingShaderContainerFormatVulkan::set_debug_info_enabled(bool p_debug_info_enabled) {
	debug_info_enabled = p_debug_info_enabled;
}

bool RenderingShaderContainerFormatVulkan::get_debug_info_enabled() const {
	return debug_info_enabled;
}

RenderingShaderContainerFormatVulkan::RenderingShaderContainerFormatVulkan() {}

RenderingShaderContainerFormatVulkan::~RenderingShaderContainerFormatVulkan() {}
