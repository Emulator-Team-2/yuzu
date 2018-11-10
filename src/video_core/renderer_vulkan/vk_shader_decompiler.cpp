// Copyright 2018 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <map>
#include <set>

#include <fmt/format.h>

#include <sirit/sirit.h>

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging/log.h"
#include "video_core/engines/shader_bytecode.h"
#include "video_core/engines/shader_header.h"
#include "video_core/renderer_vulkan/vk_shader_decompiler.h"

#pragma optimize("", off)

namespace Vulkan::VKShader::Decompiler {

using Tegra::Shader::Attribute;
using Tegra::Shader::Instruction;
using Tegra::Shader::LogicOperation;
using Tegra::Shader::OpCode;
using Tegra::Shader::Pred;
using Tegra::Shader::Register;
using Tegra::Shader::Sampler;
using Tegra::Shader::SubOp;
using ShaderStage = Maxwell3D::Regs::ShaderStage;

constexpr u32 PROGRAM_END = MAX_PROGRAM_CODE_LENGTH;
constexpr u32 PROGRAM_HEADER_SIZE = sizeof(Tegra::Shader::Header);

constexpr u32 POSITION_VARYING_LOCATION = 0;
constexpr u32 VARYING_START_LOCATION = 1;

class DecompileFail : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Describes the behaviour of code path of a given entry point and a return point.
enum class ExitMethod {
    Undetermined, ///< Internal value. Only occur when analyzing JMP loop.
    AlwaysReturn, ///< All code paths reach the return point.
    Conditional,  ///< Code path reaches the return point or an END instruction conditionally.
    AlwaysEnd,    ///< All code paths reach a END instruction.
};

/// A subroutine is a range of code refereced by a CALL, IF or LOOP instruction.
struct Subroutine {
    u32 begin;              ///< Entry point of the subroutine.
    u32 end;                ///< Return point of the subroutine.
    ExitMethod exit_method; ///< Exit method of the subroutine.
    std::set<u32> labels;   ///< Addresses refereced by JMP instructions.

    bool operator<(const Subroutine& rhs) const {
        return std::tie(begin, end) < std::tie(rhs.begin, rhs.end);
    }
};

/// Analyzes shader code and produces a set of subroutines.
class ControlFlowAnalyzer {
public:
    ControlFlowAnalyzer(const ProgramCode& program_code, u32 main_offset)
        : program_code(program_code) {

        // Recursively finds all subroutines.
        const Subroutine& program_main = AddSubroutine(main_offset, PROGRAM_END);
        if (program_main.exit_method != ExitMethod::AlwaysEnd) {
            throw DecompileFail("Program does not always end");
        }
    }

    std::set<Subroutine> GetSubroutines() {
        return std::move(subroutines);
    }

private:
    const ProgramCode& program_code;
    std::set<Subroutine> subroutines;
    std::map<std::pair<u32, u32>, ExitMethod> exit_method_map;

    /// Adds and analyzes a new subroutine if it is not added yet.
    const Subroutine& AddSubroutine(u32 begin, u32 end) {
        Subroutine subroutine{begin, end, ExitMethod::Undetermined, {}};

        const auto iter = subroutines.find(subroutine);
        if (iter != subroutines.end()) {
            return *iter;
        }

        subroutine.exit_method = Scan(begin, end, subroutine.labels);
        if (subroutine.exit_method == ExitMethod::Undetermined) {
            throw DecompileFail("Recursive function detected");
        }

        return *subroutines.insert(std::move(subroutine)).first;
    }

    /// Merges exit method of two parallel branches.
    static ExitMethod ParallelExit(ExitMethod a, ExitMethod b) {
        if (a == ExitMethod::Undetermined) {
            return b;
        }
        if (b == ExitMethod::Undetermined) {
            return a;
        }
        if (a == b) {
            return a;
        }
        return ExitMethod::Conditional;
    }

    /// Scans a range of code for labels and determines the exit method.
    ExitMethod Scan(u32 begin, u32 end, std::set<u32>& labels) {
        const auto [iter, inserted] =
            exit_method_map.emplace(std::make_pair(begin, end), ExitMethod::Undetermined);
        ExitMethod& exit_method = iter->second;
        if (!inserted)
            return exit_method;

        for (u32 offset = begin; offset != end && offset != PROGRAM_END; ++offset) {
            const Instruction instr = {program_code[offset]};
            if (const auto opcode = OpCode::Decode(instr)) {
                switch (opcode->get().GetId()) {
                case OpCode::Id::EXIT: {
                    // The EXIT instruction can be predicated, which means that the shader can
                    // conditionally end on this instruction. We have to consider the case where the
                    // condition is not met and check the exit method of that other basic block.
                    if (instr.pred.pred_index == static_cast<u64>(Pred::UnusedIndex)) {
                        return exit_method = ExitMethod::AlwaysEnd;
                    } else {
                        const ExitMethod not_met = Scan(offset + 1, end, labels);
                        return exit_method = ParallelExit(ExitMethod::AlwaysEnd, not_met);
                    }
                }
                case OpCode::Id::BRA: {
                    const u32 target = offset + instr.bra.GetBranchTarget();
                    labels.insert(target);
                    const ExitMethod no_jmp = Scan(offset + 1, end, labels);
                    const ExitMethod jmp = Scan(target, end, labels);
                    return exit_method = ParallelExit(no_jmp, jmp);
                }
                case OpCode::Id::SSY:
                case OpCode::Id::PBK: {
                    // The SSY and PBK use a similar encoding as the BRA instruction.
                    ASSERT_MSG(instr.bra.constant_buffer == 0,
                               "Constant buffer branching is not supported");
                    const u32 target = offset + instr.bra.GetBranchTarget();
                    labels.insert(target);
                    // Continue scanning for an exit method.
                    break;
                }
                }
            }
        }
        return exit_method = ExitMethod::AlwaysReturn;
    }
};

bool SpirvModule::IsSchedInstruction(u32 offset) const {
    // sched instructions appear once every 4 instructions.
    static constexpr std::size_t SchedPeriod = 4;
    u32 absolute_offset = offset - main_offset;

    return (absolute_offset % SchedPeriod) == 0;
}

Id SpirvModule::ConvertIntegerSize(Id type, Id value, Register::Size size) {
    switch (size) {
    case Register::Size::Word:
        // Default - do nothing
        return value;
    case Register::Size::Byte:
    case Register::Size::Short: {
        const Id shift = Constant(type, size == Register::Size::Byte ? 24 : 16);
        return Emit(OpShiftRightLogical(type, Emit(OpShiftLeftLogical(type, value, shift)), shift));
    }
    default:
        LOG_CRITICAL(HW_GPU, "Unimplemented conversion size {}", static_cast<u32>(size));
        UNREACHABLE();
    }
}

Id SpirvModule::GetRegister(const Register& reg, u32 elem) {
    if (reg == Register::ZeroIndex) {
        return v_float_zero;
    }
    return Emit(OpLoad(t_float, regs[reg.GetSwizzledIndex(elem)]));
}

Id SpirvModule::GetRegisterAsFloat(const Register& reg, u32 elem) {
    return GetRegister(reg, elem);
}

Id SpirvModule::GetRegisterAsInteger(const Register& reg, u32 elem, bool is_signed,
                                     Register::Size size) {
    const Id type_target{is_signed ? t_sint : t_uint};
    const Id value = Emit(OpBitcast(type_target, GetRegister(reg, elem)));
    return ConvertIntegerSize(type_target, value, size);
}

Id SpirvModule::GetInputAttribute(Attribute::Index attribute,
                                  const Tegra::Shader::IpaMode& input_mode,
                                  std::optional<Register> vertex) {
    const auto BuildComposite = [&](Id x, Id y, Id z, Id w) {
        std::vector<Id> ids = {x, y, z, w};
        return Emit(OpCompositeConstruct(t_float4, ids));
    };

    switch (attribute) {
    case Attribute::Index::TessCoordInstanceIDVertexID: {
        // TODO(Subv): Find out what the values are for the first two elements when inside a vertex
        // shader, and what's the value of the fourth element when inside a Tess Eval shader.
        ASSERT(stage == ShaderStage::Vertex);
        const Id comp_z = Emit(OpBitcast(t_float, Emit(OpLoad(t_uint, vs.instance_index))));
        const Id comp_w = Emit(OpBitcast(t_float, Emit(OpLoad(t_uint, vs.vertex_index))));
        return BuildComposite(v_float_zero, v_float_zero, comp_z, comp_w);
    }
    case Attribute::Index::Position: {
        ASSERT_MSG(stage != ShaderStage::Vertex, "Position input in a vertex shader");
        ASSERT_MSG(stage == ShaderStage::Fragment, "Input attribute stage not implemented");
        const Id frag_coord = Emit(OpLoad(t_float4, fs.frag_coord));
        return Emit(
            OpCompositeConstruct(t_float4, {Emit(OpCompositeExtract(t_float, frag_coord, {0})),
                                            Emit(OpCompositeExtract(t_float, frag_coord, {1})),
                                            Emit(OpCompositeExtract(t_float, frag_coord, {2})),
                                            Constant(t_float, 1.f)}));
        break;
    }
    case Attribute::Index::PointCoord:
    case Attribute::Index::FrontFacing:
        UNREACHABLE_MSG("Unimplemented");
    default:
        const Id composite = DeclareInputAttribute(attribute, input_mode);
        if (!composite) {
            LOG_CRITICAL(HW_GPU, "Unhandled input attribute: {}", static_cast<u32>(attribute));
            UNREACHABLE();
        }
        return Emit(OpLoad(t_float4, composite));
    }

    return v_float4_zero;
}

void SpirvModule::SetRegisterToFloat(const Register& reg, u64 elem, Id value,
                                     u64 dest_num_components, u64 value_num_components,
                                     bool is_saturated, u64 dest_elem, bool precise) {
    ASSERT_MSG(!is_saturated, "Unimplemented");

    SetRegister(reg, elem, value, dest_num_components, value_num_components, dest_elem, precise);
}

void SpirvModule::SetRegisterToInteger(const Register& reg, bool is_signed, u64 elem, Id value,
                                       u64 dest_num_components, u64 value_num_components,
                                       bool is_saturated, u64 dest_elem, Register::Size size,
                                       bool sets_cc) {
    ASSERT_MSG(!is_saturated, "Unimplemented");
    ASSERT_MSG(!sets_cc, "Unimplemented");

    const Id src_type{is_signed ? t_sint : t_uint};
    const Id src = Emit(OpBitcast(t_float, ConvertIntegerSize(src_type, value, size)));

    SetRegister(reg, elem, src, dest_num_components, value_num_components, dest_elem, false);
}

void SpirvModule::SetRegisterToInputAttibute(const Register& reg, u64 elem,
                                             Attribute::Index attribute,
                                             const Tegra::Shader::IpaMode& input_mode,
                                             std::optional<Register> vertex) {
    const Id float4_input = GetInputAttribute(attribute, input_mode, vertex);
    const Id src = Emit(OpCompositeExtract(t_float, float4_input, {static_cast<u32>(elem)}));
    SetRegisterToFloat(reg, 0, src, 1, 1);
}

void SpirvModule::SetOutputAttributeToRegister(Attribute::Index attribute, u64 elem,
                                               const Register& val_reg, const Register& buf_reg) {
    const Id dest = [&]() -> Id {
        switch (attribute) {
        case Attribute::Index::Position:
            ASSERT(stage == ShaderStage::Vertex);
            return Emit(
                OpAccessChain(t_out_float, vs.per_vertex,
                              {Constant(t_uint, 0u), Constant(t_uint, static_cast<u32>(elem))}));
        case Attribute::Index::PointSize:
            UNREACHABLE_MSG("Unimplemented built in varying");
        default:
            const auto index{static_cast<u32>(attribute) -
                             static_cast<u32>(Attribute::Index::Attribute_0)};
            if (attribute >= Attribute::Index::Attribute_0) {
                return Emit(OpAccessChain(t_out_float, DeclareOutputAttribute(index),
                                          {Constant(t_uint, static_cast<u32>(elem))}));
            }

            LOG_CRITICAL(HW_GPU, "Unhandled output attribute: {}", index);
            UNREACHABLE();
            return {};
        }
    }();
    ASSERT_MSG(dest, "Unimplemented output attribute");

    Emit(OpStore(dest, GetRegisterAsFloat(val_reg)));
}

void SpirvModule::SetRegister(const Register& reg, u64 elem, Id value, u64 dest_num_components,
                              u64 value_num_components, u64 dest_elem, bool precise) {
    if (reg == Register::ZeroIndex) {
        LOG_CRITICAL(HW_GPU, "Cannot set Register::ZeroIndex");
        UNREACHABLE();
        return;
    }

    Id dest = regs[reg.GetSwizzledIndex(elem)];
    if (dest_num_components > 1) {
        dest =
            Emit(OpAccessChain(t_prv_float, dest, {Constant(t_uint, static_cast<u32>(dest_elem))}));
    }

    Id src = value;
    if (value_num_components > 1) {
        const Id elem_index = Constant(t_uint, static_cast<u32>(elem));
        src = Emit(OpLoad(t_float, Emit(OpAccessChain(t_prv_float, dest, {elem_index}))));
    }

    // ASSERT_MSG(!precise, "Unimplemented");

    Emit(OpStore(dest, src));
}

Id SpirvModule::GetPredicateCondition(u64 index, bool negate) {
    // Index 7 is used as an 'Always True' condition.
    const Id variable = index == static_cast<u64>(Pred::UnusedIndex) ? v_true : GetPredicate(index);
    if (negate) {
        return Emit(OpLogicalNot(t_bool, variable));
    }
    return variable;
}

Id SpirvModule::GetPredicate(u64 index) {
    ASSERT(index < PRED_COUNT);
    return Emit(OpLoad(t_bool, predicates[index]));
}

Id SpirvModule::GetImmediate19(const Instruction& instr) {
    return Emit(OpBitcast(t_float, Constant(t_uint, instr.alu.GetImm20_19())));
}

Id SpirvModule::GetImmediate32(const Instruction& instr) {
    return Emit(OpBitcast(t_float, Constant(t_uint, instr.alu.GetImm20_32())));
}

Id SpirvModule::GetUniform(u64 cbuf_index, u64 offset, Id type, Register::Size size) {
    const Id cbuf = DeclareUniform(cbuf_index);
    declr_const_buffers[cbuf_index].MarkAsUsed(binding, offset, cbuf_index, stage);

    const Id subindex = Constant(t_sint, static_cast<s32>(offset / 4));
    const Id elem = Constant(t_sint, static_cast<s32>(offset % 4));
    Id value = Emit(OpLoad(
        t_float, Emit(OpAccessChain(t_ubo_float, cbuf, {Constant(t_uint, 0), subindex, elem}))));

    if (type != t_float) {
        value = Emit(OpBitcast(type, value));
    }
    return ConvertIntegerSize(type, value, size);
}

Id SpirvModule::GetUniformIndirect(u64 cbuf_index, s64 offset, Id index, Id type) {
    const Id cbuf = DeclareUniform(cbuf_index);
    declr_const_buffers[cbuf_index].MarkAsUsedIndirect(binding, cbuf_index, stage);

    const Id final_offset =
        Emit(OpIAdd(t_uint, index, Constant(t_uint, static_cast<u32>(offset / 4))));
    const Id subindex = Emit(OpUDiv(t_uint, final_offset, Constant(t_uint, 4)));
    const Id elem = Emit(OpUMod(t_uint, final_offset, Constant(t_uint, 4)));
    const Id value = Emit(OpLoad(
        t_float, Emit(OpAccessChain(t_ubo_float, cbuf, {Constant(t_uint, 0), subindex, elem}))));

    if (type == t_float) {
        return value;
    }
    return Emit(OpBitcast(type, value));
}

Id SpirvModule::DeclareUniform(u64 cbuf_index) {
    if (declr_const_buffers[cbuf_index].IsUsed()) {
        return cbufs[cbuf_index];
    }
    const Id variable = AddGlobalVariable(Name(OpVariable(t_cbuf_ubo, spv::StorageClass::Uniform),
                                               fmt::format("cbuf{}", cbuf_index)));
    Decorate(variable, spv::Decoration::Binding, {binding});
    Decorate(variable, spv::Decoration::DescriptorSet, {descriptor_set});
    return cbufs[cbuf_index] = variable;
}

Id SpirvModule::DeclareInputAttribute(Attribute::Index attribute,
                                      Tegra::Shader::IpaMode input_mode) {
    const bool is_custom =
        attribute >= Attribute::Index::Attribute_0 && attribute <= Attribute::Index::Attribute_31;

    if (declr_input_attribute.count(attribute) != 0) {
        const auto& entry = declr_input_attribute[attribute];
        if (is_custom && entry.input_mode != input_mode) {
            LOG_CRITICAL(HW_GPU, "Same Input multiple input modes");
            UNREACHABLE();
        }
        return entry.id;
    }
    const auto index{static_cast<u32>(attribute) - static_cast<u32>(Attribute::Index::Attribute_0)};
    if (!is_custom) {
        LOG_CRITICAL(HW_GPU, "Unhandled input attribute: {}", static_cast<u32>(attribute));
        UNREACHABLE();
    }

    const Id variable = AddGlobalVariable(OpVariable(t_in_float4, spv::StorageClass::Input));
    Name(variable, fmt::format("input_attr_{}", index));

    // When the stage is not vertex, the first varyings are reserved for emulation values (like
    // "position").
    const u32 offset = stage == ShaderStage::Vertex ? 0 : VARYING_START_LOCATION;
    Decorate(variable, spv::Decoration::Location, {index + offset});

    const InputAttributeEntry entry{variable, input_mode};
    declr_input_attribute.insert(std::make_pair(attribute, entry));
    interfaces.push_back(variable);
    return entry.id;
}

Id SpirvModule::DeclareOutputAttribute(u32 index) {
    if (output_attrs.count(index) != 0) {
        return output_attrs[index];
    }
    const Id variable =
        AddGlobalVariable(OpVariable(t_out_float4, spv::StorageClass::Output, v_float4_zero));
    Name(variable, fmt::format("output_attr_{}", index));
    Decorate(variable, spv::Decoration::Location, {VARYING_START_LOCATION});

    output_attrs.insert(std::make_pair(index, variable));
    interfaces.push_back(variable);
    return variable;
}

Id SpirvModule::GetOperandFloatAbsNeg(Id operand, bool abs, bool neg) {
    Id result = operand;
    if (abs) {
        result = OpFAbs(t_float, result);
    }
    if (neg) {
        result = OpFNegate(t_float, result);
    }
    return result;
}

void SpirvModule::EmitFragmentOutputsWrite() {
    ASSERT(stage == Maxwell3D::Regs::ShaderStage::Fragment);
    ASSERT_MSG(header.ps.omap.sample_mask == 0, "Samplemask write is unimplemented");

    // Write the color outputs using the data in the shader registers, disabled
    // rendertargets/components are skipped in the register assignment.
    u32 current_reg = 0;
    for (u32 render_target = 0; render_target < Maxwell3D::Regs::NumRenderTargets;
         ++render_target) {
        // TODO(Subv): Figure out how dual-source blending is configured in the Switch.
        for (u32 component = 0; component < 4; ++component) {
            if (!header.ps.IsColorComponentOutputEnabled(render_target, component)) {
                continue;
            }
            const Id target = Emit(OpAccessChain(t_out_float, fs.frag_colors[render_target],
                                                 {Constant(t_uint, component)}));
            Emit(OpStore(target, GetRegisterAsFloat(current_reg)));
            ++current_reg;
        }
    }

    if (header.ps.omap.depth) {
        // The depth output is always 2 registers after the last color output, and current_reg
        // already contains one past the last color register.
        Emit(OpStore(fs.frag_depth,
                     GetRegisterAsFloat(static_cast<Tegra::Shader::Register>(current_reg) + 1)));
    }
}

Id SpirvModule::Generate(const std::set<Subroutine> subroutines) {
    std::vector<Id> exec_code;

    // Add definitions for all subroutines
    for (const auto& subroutine : subroutines) {
        const Id function =
            Emit(OpFunction(t_bool, spv::FunctionControlMask::Inline, t_bool_function));

        // Store function call code.
        const Id function_call = OpFunctionCall(t_bool, function);
        exec_code.push_back(function_call);
        if (subroutine.exit_method == ExitMethod::AlwaysEnd) {
            exec_code.push_back(OpReturnValue(v_true));
        } else if (subroutine.exit_method == ExitMethod::Conditional) {
            const Id true_label = OpLabel();
            const Id false_label = OpLabel();
            exec_code.push_back(OpBranchConditional(function_call, true_label, false_label));
            exec_code.push_back(true_label);
            exec_code.push_back(OpReturnValue(v_true));
            exec_code.push_back(false_label);
        }

        // Generate branch target labels.
        std::set<u32> labels = subroutine.labels;
        labels.insert(subroutine.begin);

        std::map<u32, Id> labels_ids;
        for (const auto label : labels) {
            labels_ids.insert(
                std::make_pair(label, Name(OpLabel(), fmt::format("code_0x{:04x}", label))));
        }

        for (const auto label_addr : labels) {
            Emit(labels_ids[label_addr]);

            const auto next_it = labels.lower_bound(label_addr + 1);
            const u32 next_label = next_it == labels.end() ? subroutine.end : *next_it;

            const u32 compile_end = CompileRange(label_addr, next_label);
            if (compile_end > next_label && compile_end != PROGRAM_END) {
                // This happens only when there is a label inside a IF/LOOP block
                UNREACHABLE();
            }
        }

        Emit(OpFunctionEnd());
    }

    Id exec_function = Emit(Name(
        OpFunction(t_bool, spv::FunctionControlMask::Inline, t_bool_function), "exec_function"));
    Emit(OpLabel());
    for (const auto instr : exec_code) {
        Emit(instr);
    }
    Emit(OpFunctionEnd());

    return exec_function;
}

u32 SpirvModule::CompileRange(u32 begin, u32 end) {
    u32 program_counter;
    for (program_counter = begin; program_counter < (begin > end ? PROGRAM_END : end);) {
        program_counter = CompileInstr(program_counter);
    }
    return program_counter;
}

u32 SpirvModule::CompileInstr(u32 offset) {
    // Ignore sched instructions when generating code.
    if (IsSchedInstruction(offset)) {
        return offset + 1;
    }

    const Instruction instr = {program_code[offset]};
    const auto opcode = OpCode::Decode(instr);

    // Decoding failure
    if (!opcode) {
        LOG_CRITICAL(HW_GPU, "Unhandled instruction: {0:x}", instr.value);
        UNREACHABLE();
        return offset + 1;
    }

    Name(Emit(OpUndef(t_void)),
         fmt::format("{}_{}_0x{:016x}", offset, opcode->get().GetName(), instr.value));

    ASSERT_MSG(instr.pred.full_pred != Pred::NeverExecute,
               "NeverExecute predicate not implemented");

    // Some instructions (like SSY) don't have a predicate field, they are always
    // unconditionally executed.
    const bool can_be_predicated = OpCode::IsPredicatedInstruction(opcode->get().GetId());
    const Id no_exec_label = OpLabel();

    if (can_be_predicated && instr.pred.pred_index != static_cast<u64>(Pred::UnusedIndex)) {
        const Id exec_label = OpLabel();
        const Id cond = GetPredicateCondition(instr.pred.pred_index, instr.negate_pred != 0);
        Emit(OpBranchConditional(cond, exec_label, no_exec_label));
        Emit(exec_label);
    }

    switch (opcode->get().GetType()) {
    case OpCode::Type::Arithmetic: {
        Id op_a = GetRegisterAsFloat(instr.gpr8);
        Id op_b = [&]() {
            if (instr.is_b_imm) {
                return GetImmediate19(instr);
            } else {
                if (instr.is_b_gpr) {
                    return GetRegisterAsFloat(instr.gpr20);
                } else {
                    return GetUniform(instr.cbuf34.index, instr.cbuf34.offset, t_float);
                }
            }
        }();

        switch (opcode->get().GetId()) {
        case OpCode::Id::MUFU: {
            op_a = GetOperandFloatAbsNeg(op_a, instr.alu.abs_a, instr.alu.negate_a);
            const Id result = [&]() {
                switch (instr.sub_op) {
                case SubOp::Rcp:
                    return Emit(OpFDiv(t_float, Constant(t_float, 1.f), op_a));
                case SubOp::Cos:
                case SubOp::Sin:
                case SubOp::Ex2:
                case SubOp::Lg2:
                case SubOp::Rsq:
                case SubOp::Sqrt:
                default:
                    LOG_CRITICAL(HW_GPU, "Unhandled MUFU sub op: {0:x}",
                                 static_cast<unsigned>(instr.sub_op.Value()));
                    UNREACHABLE();
                }
            }();
            SetRegisterToFloat(instr.gpr0, 0, result, 1, 1, instr.alu.saturate_d, 0, true);
            break;
        }
        default: {
            LOG_CRITICAL(HW_GPU, "Unhandled arithmetic instruction: {}", opcode->get().GetName());
            UNREACHABLE();
        }
        }
        break;
    }
    case OpCode::Type::ArithmeticImmediate: {
        switch (opcode->get().GetId()) {
        case OpCode::Id::MOV32_IMM: {
            SetRegisterToFloat(instr.gpr0, 0, GetImmediate32(instr), 1, 1);
            break;
        }
        }
        break;
    }
    case OpCode::Type::Shift: {
        Id op_a = GetRegisterAsInteger(instr.gpr8, 0, true);
        const Id op_b = [&]() {
            if (instr.is_b_imm) {
                return Constant(t_sint, instr.alu.GetSignedImm20_20());
            } else {
                if (instr.is_b_gpr) {
                    return GetRegisterAsInteger(instr.gpr20);
                } else {
                    return GetUniform(instr.cbuf34.index, instr.cbuf34.offset, t_uint);
                }
            }
        }();

        switch (opcode->get().GetId()) {
        case OpCode::Id::SHR_C:
        case OpCode::Id::SHR_R:
        case OpCode::Id::SHR_IMM: {
            if (!instr.shift.is_signed) {
                // Logical shift right
                op_a = Emit(OpBitcast(t_uint, op_a));
            }

            // Cast to int is superfluous for arithmetic shift, it's only for a logical shift
            const Id value =
                Emit(OpBitcast(t_sint, Emit(OpShiftRightArithmetic(t_uint, op_a, op_b))));
            SetRegisterToInteger(instr.gpr0, true, 0, value, 1, 1);
            break;
        }
        case OpCode::Id::SHL_C:
        case OpCode::Id::SHL_R:
        case OpCode::Id::SHL_IMM:
            SetRegisterToInteger(instr.gpr0, true, 0, Emit(OpShiftLeftLogical(t_sint, op_a, op_b)),
                                 1, 1);
            break;
        default: {
            LOG_CRITICAL(HW_GPU, "Unhandled shift instruction: {}", opcode->get().GetName());
            UNREACHABLE();
        }
        }
        break;
    }
    case OpCode::Type::Memory: {
        switch (opcode->get().GetId()) {
        case OpCode::Id::LD_A: {
            // Note: Shouldn't this be interp mode flat? As in no interpolation made.
            ASSERT_MSG(instr.gpr8.Value() == Register::ZeroIndex,
                       "Indirect attribute loads are not supported");
            ASSERT_MSG((instr.attribute.fmt20.immediate.Value() % sizeof(u32)) == 0,
                       "Unaligned attribute loads are not supported");

            Tegra::Shader::IpaMode input_mode{Tegra::Shader::IpaInterpMode::Perspective,
                                              Tegra::Shader::IpaSampleMode::Default};

            u64 next_element = instr.attribute.fmt20.element;
            u64 next_index = static_cast<u64>(instr.attribute.fmt20.index.Value());

            const auto LoadNextElement = [&](u32 reg_offset) {
                SetRegisterToInputAttibute(instr.gpr0.Value() + reg_offset, next_element,
                                           static_cast<Attribute::Index>(next_index), input_mode,
                                           instr.gpr39.Value());

                // Load the next attribute element into the following register. If the element
                // to load goes beyond the vec4 size, load the first element of the next
                // attribute.
                next_element = (next_element + 1) % 4;
                next_index = next_index + (next_element == 0 ? 1 : 0);
            };

            const u32 num_words = static_cast<u32>(instr.attribute.fmt20.size.Value()) + 1;
            for (u32 reg_offset = 0; reg_offset < num_words; ++reg_offset) {
                LoadNextElement(reg_offset);
            }
            break;
        }
        case OpCode::Id::LD_C: {
            ASSERT_MSG(instr.ld_c.unknown == 0, "Unimplemented");

            const Id index =
                Emit(OpBitwiseAnd(t_uint,
                                  Emit(OpUDiv(t_uint, GetRegisterAsInteger(instr.gpr8, 0, false),
                                              Constant(t_uint, 4))),
                                  Constant(t_uint, MAX_CONSTBUFFER_ELEMENTS - 1)));

            const Id op_a =
                GetUniformIndirect(instr.cbuf36.index, instr.cbuf36.offset + 0, index, t_float);

            switch (instr.ld_c.type.Value()) {
            case Tegra::Shader::UniformType::Single:
                SetRegisterToFloat(instr.gpr0, 0, op_a, 1, 1);
                break;

            case Tegra::Shader::UniformType::Double: {
                const Id op_b =
                    GetUniformIndirect(instr.cbuf36.index, instr.cbuf36.offset + 4, index, t_float);
                SetRegisterToFloat(instr.gpr0, 0, op_a, 1, 1);
                SetRegisterToFloat(instr.gpr0.Value() + 1, 0, op_b, 1, 1);
                break;
            }
            default:
                LOG_CRITICAL(HW_GPU, "Unhandled type: {}",
                             static_cast<u32>(instr.ld_c.type.Value()));
                UNREACHABLE();
            }
            break;
        }
        case OpCode::Id::ST_A: {
            ASSERT_MSG(instr.gpr8.Value() == Register::ZeroIndex,
                       "Indirect attribute loads are not supported");
            ASSERT_MSG((instr.attribute.fmt20.immediate.Value() % sizeof(u32)) == 0,
                       "Unaligned attribute loads are not supported");

            u64 next_element = instr.attribute.fmt20.element;
            auto next_index = static_cast<u64>(instr.attribute.fmt20.index.Value());

            const auto StoreNextElement = [&](u32 reg_offset) {
                SetOutputAttributeToRegister(static_cast<Attribute::Index>(next_index),
                                             next_element, instr.gpr0.Value() + reg_offset,
                                             instr.gpr39.Value());

                // Load the next attribute element into the following register. If the element
                // to load goes beyond the vec4 size, load the first element of the next
                // attribute.
                next_element = (next_element + 1) % 4;
                next_index = next_index + (next_element == 0 ? 1 : 0);
            };

            const u32 num_words = static_cast<u32>(instr.attribute.fmt20.size.Value()) + 1;
            for (u32 reg_offset = 0; reg_offset < num_words; ++reg_offset) {
                StoreNextElement(reg_offset);
            }

            break;
        }
        default: {
            LOG_CRITICAL(HW_GPU, "Unhandled memory instruction: {}", opcode->get().GetName());
            UNREACHABLE();
        }
        }
        break;
    }
    default: {
        switch (opcode->get().GetId()) {
        case OpCode::Id::EXIT: {
            if (stage == ShaderStage::Fragment) {
                EmitFragmentOutputsWrite();
            }

            switch (instr.flow.cond) {
            case Tegra::Shader::FlowCondition::Always:
                Emit(OpReturnValue(v_true));
                if (instr.pred.pred_index == static_cast<u64>(Pred::UnusedIndex)) {
                    // If this is an unconditional exit then just end processing here,
                    // otherwise we have to account for the possibility of the condition
                    // not being met, so continue processing the next instruction.
                    offset = PROGRAM_END - 1;
                }
                break;

            case Tegra::Shader::FlowCondition::Fcsm_Tr:
                // TODO(bunnei): What is this used for? If we assume this conditon is not
                // satisifed, dual vertex shaders in Farming Simulator make more sense
                LOG_CRITICAL(HW_GPU, "Skipping unknown FlowCondition::Fcsm_Tr");
                break;

            default:
                LOG_CRITICAL(HW_GPU, "Unhandled flow condition: {}",
                             static_cast<u32>(instr.flow.cond.Value()));
                UNREACHABLE();
            }
            break;
        }
        case OpCode::Id::IPA: {
            const auto& attribute = instr.attribute.fmt28;
            const auto& reg = instr.gpr0;

            const Tegra::Shader::IpaMode input_mode{instr.ipa.interp_mode.Value(),
                                                    instr.ipa.sample_mode.Value()};
            SetRegisterToInputAttibute(reg, attribute.element, attribute.index, input_mode);

            if (instr.ipa.saturate) {
                SetRegisterToFloat(reg, 0, GetRegisterAsFloat(reg), 1, 1, true);
            }
            break;
        }
        default: {
            LOG_CRITICAL(HW_GPU, "Unhandled instruction: {}", opcode->get().GetName());
            UNREACHABLE();
        }
        }
    }
    }

    // Close the predicate condition branch.
    if (can_be_predicated && instr.pred.pred_index != static_cast<u64>(Pred::UnusedIndex)) {
        Emit(OpBranch(no_exec_label));
        Emit(no_exec_label);
    }

    return offset + 1;
}

void SpirvModule::DeclareVariables() {
    const auto Declare = [&](std::vector<Id>& vector, std::size_t count, Id type,
                             const char* fmt_expr) {
        vector.resize(count);
        for (std::size_t i = 0; i < vector.size(); ++i) {
            vector[i] = AddGlobalVariable(
                Name(OpVariable(type, spv::StorageClass::Private), fmt::format(fmt_expr, i)));
        }
    };

    Declare(regs, REGISTER_COUNT, t_prv_float, "gpr{}");
    Declare(predicates, PRED_COUNT, t_prv_bool, "pred{}");
}

void SpirvModule::DeclareBuiltIns() {
    const auto Declare = [&](Id type, spv::BuiltIn builtin, const std::string& name) {
        const Id id = Name(AddGlobalVariable(OpVariable(type, spv::StorageClass::Input)), name);
        Decorate(id, spv::Decoration::BuiltIn, {static_cast<u32>(builtin)});
        interfaces.push_back(id);
        return id;
    };

    switch (stage) {
    case ShaderStage::Vertex:
        vs.per_vertex_struct = Name(OpTypeStruct({t_float4}), "per_vertex_struct");
        Decorate(vs.per_vertex_struct, spv::Decoration::Block);
        MemberDecorate(vs.per_vertex_struct, 0, spv::Decoration::BuiltIn,
                       {static_cast<u32>(spv::BuiltIn::Position)});
        MemberName(vs.per_vertex_struct, 0, "host_position");

        vs.per_vertex = OpVariable(OpTypePointer(spv::StorageClass::Output, vs.per_vertex_struct),
                                   spv::StorageClass::Output);
        AddGlobalVariable(Name(vs.per_vertex, "per_vertex"));
        interfaces.push_back(vs.per_vertex);

        vs.vertex_index = Declare(t_in_uint, spv::BuiltIn::VertexIndex, "vertex_index");
        vs.instance_index = Declare(t_in_uint, spv::BuiltIn::InstanceIndex, "instance_index");
        break;
    case ShaderStage::Fragment:
        fs.frag_coord = Declare(t_in_float4, spv::BuiltIn::FragCoord, "frag_coord");
        break;
    default:
        UNREACHABLE_MSG("Unimplemented");
    }
}

void SpirvModule::DeclareFragmentOutputs() {
    ASSERT(stage == Maxwell3D::Regs::ShaderStage::Fragment);

    for (u32 rt = 0; rt < fs.frag_colors.size(); ++rt) {
        const Id variable = AddGlobalVariable(OpVariable(t_out_float4, spv::StorageClass::Output));
        Name(variable, fmt::format("frag_color{}", rt));
        Decorate(variable, spv::Decoration::Location, {rt});

        fs.frag_colors[rt] = variable;
        interfaces.push_back(variable);
    }

    if (header.ps.omap.depth) {
        fs.frag_depth = AddGlobalVariable(OpVariable(t_out_float, spv::StorageClass::Output));
        Name(fs.frag_depth, "frag_depth");
        Decorate(fs.frag_depth, spv::Decoration::BuiltIn,
                 {static_cast<u32>(spv::BuiltIn::FragDepth)});

        interfaces.push_back(fs.frag_depth);
    }
}

SpirvModule::SpirvModule(const ProgramCode& program_code, u32 main_offset, ShaderStage stage)
    : Sirit::Module(0x00010000), program_code(program_code), main_offset(main_offset), stage(stage),
      descriptor_set(static_cast<u32>(stage)) {

    Decorate(t_cbuf_struct, spv::Decoration::Block);
    MemberDecorate(t_cbuf_struct, 0, spv::Decoration::Offset, {0});
    MemberName(t_cbuf_struct, 0, "cbuf_array");

    std::memcpy(&header, program_code.data(), sizeof(Tegra::Shader::Header));

    DeclareVariables();
    DeclareBuiltIns();

    if (stage == ShaderStage::Fragment) {
        DeclareFragmentOutputs();
    }
}

SpirvModule::~SpirvModule() = default;

Id SpirvModule::Decompile() {
    try {
        const auto subroutines = ControlFlowAnalyzer(program_code, main_offset).GetSubroutines();
        return Generate(subroutines);
    } catch (const DecompileFail& exception) {
        LOG_ERROR(HW_GPU, "Shader decompilation failed: {}", exception.what());
    }
    return {};
}

} // namespace Vulkan::VKShader::Decompiler