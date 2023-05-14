#include "disassembler.h"

// Very WIP Multi-Threaded Recursive Decompiler

using namespace binlex;
using json = nlohmann::json;

cs_arch Disassembler::arch;
cs_mode Disassembler::mode;

//from https://github.com/capstone-engine/capstone/blob/master/include/capstone/x86.h

#define X86_REL_ADDR(insn) (((insn).detail->x86.operands[0].type == X86_OP_IMM) \
	? (uint64_t)((insn).detail->x86.operands[0].imm) \
	: (((insn).address + (insn).size) + (uint64_t)(insn).detail->x86.disp))

Disassembler::Disassembler(const binlex::File &firef) : DisassemblerBase(firef) {
    // Set Decompiler Architecture
    switch(file_reference.binary_arch){
        case BINARY_ARCH_X86:
        case BINARY_ARCH_X86_64:
            arch = CS_ARCH_X86;
            break;
        default:
            PRINT_ERROR_AND_EXIT("[x] failed to set decompiler architecture\n");
    }
    // Set Decompiler Mode
    switch(file_reference.binary_mode){
        case BINARY_MODE_32:
            mode = CS_MODE_32;
            break;
        case BINARY_MODE_64:
            mode = CS_MODE_64;
            break;
        default:
            PRINT_ERROR_AND_EXIT("[x] failed to set decompiler mode\n");
    }
    // Append the Function Queue
    for (uint32_t i = 0; i < file_reference.total_exec_sections; i++){
        std::set<uint64_t> tmp = file_reference.sections[i].functions;
        AppendQueue(tmp, DISASSEMBLER_OPERAND_TYPE_FUNCTION, i);
    }
    for (int i = 0; i < BINARY_MAX_SECTIONS; i++) {
        sections[i].offset = 0;
        sections[i].data = NULL;
        sections[i].data_size = 0;
    }
}

void Disassembler::AppendTrait(struct Trait *trait, struct Section *sections, uint index){
    struct Trait new_elem_trait = *trait;
    sections[index].traits.push_back(new_elem_trait);
 }

json Disassembler::GetTrait(struct Trait &trait){
    json data;
    data["type"] = trait.type;
    data["corpus"] = g_args.options.corpus;
    data["tags"] = g_args.options.tags;
    data["mode"] = g_args.options.mode;
    data["bytes"] = trait.bytes;
    data["trait"] = trait.trait;
    data["edges"] = trait.edges;
    data["blocks"] = trait.blocks;
    data["instructions"] = trait.instructions;
    data["size"] = trait.size;
    data["offset"] = trait.offset;
    data["bytes_entropy"] = trait.bytes_entropy;
    data["bytes_sha256"] = trait.bytes_sha256;
    string bytes_tlsh = TraitToTLSH(trait.bytes);
    if (bytes_tlsh.length() > 0){
        data["bytes_tlsh"] = bytes_tlsh;
    } else {
        data["bytes_tlsh"] = nullptr;
    }
    data["trait_sha256"] = trait.trait_sha256;
    string trait_tlsh = TraitToTLSH(trait.trait);
    if (trait_tlsh.length() > 0){
        data["trait_tlsh"] = trait_tlsh;
    } else {
        data["trait_tlsh"] = nullptr;
    }
    data["trait_entropy"] = trait.trait_entropy;
    data["invalid_instructions"] = trait.invalid_instructions;
    data["cyclomatic_complexity"] = trait.cyclomatic_complexity;
    data["average_instructions_per_block"] = trait.average_instructions_per_block;
    return data;
}

vector<json> Disassembler::GetTraits(){
    vector<json> traitsjson;
    for (int i = 0; i < BINARY_MAX_SECTIONS; i++){
        if (sections[i].data != NULL){
            for (size_t j = 0; j < sections[i].traits.size(); j++){
                json jdata(GetTrait(sections[i].traits[j]));
                traitsjson.push_back(jdata);
            }
        }
    }
    return traitsjson;
}

void * Disassembler::CreateTraitsForSection(uint index) {
    disasm_t myself;

    struct Trait b_trait;
    struct Trait f_trait;

    PRINT_DEBUG("----------\nHandling section %u\n----------\n", index);

    b_trait.type = "block";
    ClearTrait(&b_trait);
    f_trait.type = "function";
    ClearTrait(&f_trait);

    myself.error = cs_open(arch, mode, &myself.handle);
    if (myself.error != CS_ERR_OK) {
        return NULL;
    }
    myself.error = cs_option(myself.handle, CS_OPT_DETAIL, CS_OPT_ON);
    if (myself.error != CS_ERR_OK) {
        return NULL;
    }

    cs_insn *insn = cs_malloc(myself.handle);
    while (!sections[index].discovered.empty()){
        uint64_t address = 0;

        PRINT_DEBUG("discovered size = %u\n", (uint32_t)sections[index].discovered.size());
        PRINT_DEBUG("visited size = %u\n",    (uint32_t)sections[index].visited.size());
        PRINT_DEBUG("functions size = %u\n",  (uint32_t)sections[index].functions.size());
        PRINT_DEBUG("blocks size = %u\n",  (uint32_t)sections[index].blocks.size());

        address = sections[index].discovered.front();
        sections[index].discovered.pop();
        sections[index].visited[address] = DISASSEMBLER_VISITED_ANALYZED;

        myself.pc = address;
        myself.code = (uint8_t *)((uint8_t *)sections[index].data + address);
        myself.code_size = sections[index].data_size + address;

        //bool block = IsBlock(sections[index].blocks, address);
        bool function = IsFunction(sections[index].functions, address);
        uint suspicious_instructions = 0;

        while(true) {
            uint edges = 0;

            if (myself.pc >= sections[index].data_size) {
                break;
            }

            bool result = cs_disasm_iter(myself.handle, &myself.code, &myself.code_size, &myself.pc, insn);

            if (result != true){
                // Error with disassembly, not a valid basic block,
                PRINT_DEBUG("*** Decompile error rejected block: 0x%" PRIx64 "\n", myself.pc);
                ClearTrait(&b_trait);
                ClearTrait(&f_trait);
                myself.code = (uint8_t *)((uint8_t *)myself.code + 1);
                myself.code_size +=1;
                myself.pc +=1;
                break;
            }

            // Check for suspicious instructions and count them
            if (IsSuspiciousInsn(insn) == true){
                suspicious_instructions += 1;
            }

            // If there are too many suspicious instructions in the bb discard it
            // TODO: Make this configurable as an argument
            if (suspicious_instructions > 2){
                PRINT_DEBUG("*** Suspicious instructions rejected block: 0x%" PRIx64 "\n", insn->address);
                ClearTrait(&b_trait);
                ClearTrait(&f_trait);
                break;
            }

            b_trait.instructions++;
            f_trait.instructions++;

            // Need to Wildcard Traits Here
            if (IsWildcardInsn(insn) == true){
                b_trait.trait = b_trait.trait + Wildcards(insn->size) + " ";
                f_trait.trait = f_trait.trait + Wildcards(insn->size) + " ";
            } else {
                b_trait.trait = b_trait.trait + WildcardInsn(insn) + " ";
                f_trait.trait = f_trait.trait + WildcardInsn(insn) + " ";
            }
            b_trait.bytes = b_trait.bytes + HexdumpBE(insn->bytes, insn->size) + " ";
            f_trait.bytes = f_trait.bytes + HexdumpBE(insn->bytes, insn->size) + " ";
            edges = GetInsnEdges(insn);
            b_trait.edges = b_trait.edges + edges;
            f_trait.edges = f_trait.edges + edges;
            if (edges > 0){
                b_trait.blocks++;
                f_trait.blocks++;
            }

            CollectInsn(insn, sections, index);

            PRINT_DEBUG("address=0%" PRIx64 ",block=%d,function=%d,queue=%ld,instruction=%s\t%s\n", insn->address,IsBlock(sections[index].blocks, insn->address), IsFunction(sections[index].functions, insn->address), sections[index].discovered.size(), insn->mnemonic, insn->op_str);

            if (IsJumpInsn(insn) == true){
                b_trait.trait = TrimRight(b_trait.trait);
                b_trait.bytes = TrimRight(b_trait.bytes);
                b_trait.size = GetByteSize(b_trait.bytes);
                b_trait.offset = sections[index].offset + myself.pc - b_trait.size;
                AppendTrait(&b_trait, sections, index);
                ClearTrait(&b_trait);
                if (function == false){
                    ClearTrait(&f_trait);
                    break;
                }
            }
            if (IsRetInsn(insn) == true){
                b_trait.xrefs.insert(address);
                b_trait.trait = TrimRight(b_trait.trait);
                b_trait.bytes = TrimRight(b_trait.bytes);
                b_trait.size = GetByteSize(b_trait.bytes);
                b_trait.offset = sections[index].offset + myself.pc - b_trait.size;
                AppendTrait(&b_trait, sections, index);
                ClearTrait(&b_trait);
                f_trait.xrefs.insert(address);
                f_trait.trait = TrimRight(f_trait.trait);
                f_trait.bytes = TrimRight(f_trait.bytes);
                f_trait.size = GetByteSize(f_trait.bytes);
                f_trait.offset = sections[index].offset + myself.pc - f_trait.size;
                AppendTrait(&f_trait, sections, index);
                ClearTrait(&f_trait);
                break;
            }
        }
    }
    cs_free(insn, 1);
    cs_close(&myself.handle);
    return NULL;
}

void * Disassembler::FinalizeTrait(struct Trait &trait){
    if (trait.blocks == 0 &&
        (strcmp(trait.type.c_str(), "function") == 0 ||
        strcmp(trait.type.c_str(), "block") == 0)){
        trait.blocks++;
    }
    trait.bytes_entropy = Entropy(string(trait.bytes));
    trait.trait_entropy = Entropy(string(trait.trait));
    trait.trait_sha256 = SHA256((char *)trait.trait.c_str());
    trait.bytes_sha256 = SHA256((char *)trait.bytes.c_str());
    if (strcmp(trait.type.c_str(), (char *)"block") == 0){
        trait.cyclomatic_complexity = trait.edges - 1 + 2;
        trait.average_instructions_per_block = trait.instructions / 1;
    }
    if (strcmp(trait.type.c_str(), (char *)"function") == 0){
        trait.cyclomatic_complexity = trait.edges - trait.blocks + 2;
        trait.average_instructions_per_block = trait.instructions / trait.blocks;
    }
    return NULL;
}

void Disassembler::ClearTrait(struct Trait *trait){
    trait->bytes.clear();
    trait->edges = 0;
    trait->instructions = 0;
    trait->blocks = 0;
    trait->offset = 0;
    trait->size = 0;
    trait->invalid_instructions = 0;
    trait->trait.clear();
    trait->trait.clear();
    trait->bytes_sha256.clear();
    trait->trait_sha256.clear();
    trait->xrefs.clear();
}

void Disassembler::AppendQueue(set<uint64_t> &addresses, DISASSEMBLER_OPERAND_TYPE operand_type, uint index){
    PRINT_DEBUG("List of queued addresses for section %u correponding to found functions: ", index);
    for (auto it = addresses.begin(); it != addresses.end(); ++it){
        uint64_t tmp_addr = *it;
        sections[index].discovered.push(tmp_addr);
        sections[index].visited[tmp_addr] = DISASSEMBLER_VISITED_QUEUED;
        switch(operand_type){
            case DISASSEMBLER_OPERAND_TYPE_BLOCK:
                AddDiscoveredBlock(tmp_addr, sections, index);
                break;
            case DISASSEMBLER_OPERAND_TYPE_FUNCTION:
                AddDiscoveredFunction(tmp_addr, sections, index);
                break;
            default:
                break;
        }
        PRINT_DEBUG("0x%" PRIu64 " ", tmp_addr);
    }
    PRINT_DEBUG("\n");
}

void Disassembler::LinearDisassemble(void* data, size_t data_size, size_t offset, uint index) {
    // This function is intended to perform a preliminary quick linear disassembly pass of the section
    // and initially populate the discovered queue with addressed that may not be found via recursive disassembly.
    //
    // TODO: This algorithm is garbage and creates a lot of false positives, it should be replaced with a proper
    // linear pass that can differentiate data and code.
    // See research linked here: https://github.com/c3rb3ru5d3d53c/binlex/issues/42#issuecomment-1110479885
    //
    // * The Algorithm *
    // - Disassemble each instruction sequentially
    // - Track the state of the disassembly (valid / invalid)
    // - The state is set to invalid for nops, traps, privileged instructions, and errors
    // - When a jmp (conditional or unconditional) is encountered if the state is valid begin counting valid “blocks”
    // - When three consecutive blocks are found push the jmp addresses onto the queue
    // - When a jmp (conditional or unconditional) is encountered if the state is invalid reset to valid and begin tracking blocks
    //
    // * Weaknesses *
    // - We don’t collect calls (these are assumed to be collected in the recursive disassembler) (DONE)
    // - We don’t reset the state on ret or call instructions possibly missing some valid blocks (DONE)
    // - We don’t collect the next address after a jmp (next block) missing some valid blocks (effective?)
    // - Even with the filtering we will still add some number of addresses that are from invalid jmp institutions

    disasm_t disasm;

    PRINT_DEBUG("LinearDisassemble: Started at offset = 0x%" PRIu64 " data_size = %" PRIu64 " bytes\n", offset, data_size);

    if(cs_open(arch, mode, &disasm.handle) != CS_ERR_OK) {
        PRINT_ERROR_AND_EXIT("[x] LinearDisassembly failed to init capstone\n");
    }

    if (cs_option(disasm.handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) {
        PRINT_ERROR_AND_EXIT("[x] LinearDisassembly failed to set capstone options\n");
    }

    cs_insn *cs_ins = cs_malloc(disasm.handle);
    disasm.pc = offset;
    disasm.code = (uint8_t *)((uint8_t *)data);
    disasm.code_size = data_size + disasm.pc;
    bool valid_block = true;
    uint64_t valid_block_count = 1;
    uint64_t jmp_address_1 = 0;
    uint64_t jmp_address_2 = 0;

    while(disasm.pc < disasm.code_size){
        if (!cs_disasm_iter(disasm.handle, &disasm.code, &disasm.code_size, &disasm.pc, cs_ins)){
            PRINT_DEBUG("LinearDisassemble: 0x%" PRIu64 ": Disassemble ERROR\n", disasm.pc);
            disasm.pc += 1;
            disasm.code_size -= 1;
            disasm.code = (uint8_t *)((uint8_t *)disasm.code + 1);
            valid_block = false;
            valid_block_count = 0;
            continue;
        }
        PRINT_DEBUG("LinearDisassemble: 0x%" PRIu64 ": %s\t%s\n", cs_ins->address, cs_ins->mnemonic, cs_ins->op_str);

        if (IsSuspiciousInsn(cs_ins) == true){
            PRINT_DEBUG("LinearDisassemble: Suspicious instruction at 0x%" PRIu64 "\n", cs_ins->address);
            valid_block = false;
            valid_block_count = 0;
            continue;
        }

        if (IsCallInsn(cs_ins) == true && valid_block_count >= 2){
            CollectInsn(cs_ins, sections, index);
        }

        if (IsJumpInsn(cs_ins) == true){
            if (valid_block){
                if (valid_block_count == 1) {
                    jmp_address_2 =  X86_REL_ADDR(*cs_ins);
                } else if (valid_block_count == 2) {
                    PRINT_DEBUG("LinearDisassemble: Found three consecutive valid blocks adding jmp addresses\n");
                    AddDiscoveredBlock(jmp_address_1, sections, index);
                    AddDiscoveredBlock(jmp_address_2, sections, index);
                    CollectInsn(cs_ins, sections, index);
                }
                valid_block_count++;
            } else {
                valid_block = true;
                valid_block_count = 1;
                jmp_address_1 = X86_REL_ADDR(*cs_ins);
            }
        }
    }
    cs_free(cs_ins, 1);
    cs_close(&disasm.handle);
};

void Disassembler::Disassemble() {
    for (uint32_t i = 0; i < file_reference.total_exec_sections; i++){
        sections[i].offset = file_reference.sections[i].offset;
        sections[i].data = file_reference.sections[i].data;
        sections[i].data_size = file_reference.sections[i].size;
        LinearDisassemble(sections[i].data, sections[i].data_size, sections[i].offset, i);
        CreateTraitsForSection(i);
        for (size_t j = 0; j < sections[i].traits.size(); ++j) {
            FinalizeTrait(sections[i].traits[j]);
        }
    }
}

string Disassembler::WildcardInsn(cs_insn *insn){
    string bytes = HexdumpBE(insn->bytes, insn->size);
    string trait = bytes;
    for (int j = 0; j < insn->detail->x86.op_count; j++){
        cs_x86_op operand = insn->detail->x86.operands[j];
        switch(operand.type){
            case X86_OP_MEM:
                {
                    if (operand.mem.disp != 0){
                        trait = WildcardTrait(bytes, HexdumpBE(&operand.mem.disp, sizeof(uint64_t)));
                    }
                    break;
                }
            default:
                break;
        }
    }
    return TrimRight(trait);
}

bool Disassembler::IsVisited(map<uint64_t, DISASSEMBLER_VISITED> &visited, uint64_t address) {
    return visited.find(address) != visited.end();
}

bool Disassembler::IsNopInsn(cs_insn *ins){
    switch(ins->id) {
        case X86_INS_NOP:
        case X86_INS_FNOP:
            return true;
        default:
            return false;
    }
}

bool Disassembler::IsPaddingInsn(cs_insn *insn){
    return IsNopInsn(insn) ||
        (IsSemanticNopInsn(insn) && (file_reference.binary_type != BINARY_TYPE_PE)) ||
        (IsTrapInsn(insn) && (file_reference.binary_type == BINARY_TYPE_PE));
}

bool Disassembler::IsSemanticNopInsn(cs_insn *ins){
    cs_x86 *x86;

    /* XXX: to make this truly platform-independent, we need some real
     * semantic analysis, but for now checking known cases is sufficient */

    x86 = &ins->detail->x86;
    switch(ins->id) {
        case X86_INS_MOV:
            /* mov reg,reg */
            if((x86->op_count == 2)
                && (x86->operands[0].type == X86_OP_REG)
                && (x86->operands[1].type == X86_OP_REG)
                && (x86->operands[0].reg == x86->operands[1].reg)) {
                return true;
            }
            return false;
        case X86_INS_XCHG:
            /* xchg reg,reg */
            if((x86->op_count == 2)
                && (x86->operands[0].type == X86_OP_REG)
                && (x86->operands[1].type == X86_OP_REG)
                && (x86->operands[0].reg == x86->operands[1].reg)) {
                return true;
            }
            return false;
        case X86_INS_LEA:
            /* lea        reg,[reg + 0x0] */
            if((x86->op_count == 2)
                && (x86->operands[0].type == X86_OP_REG)
                && (x86->operands[1].type == X86_OP_MEM)
                && (x86->operands[1].mem.segment == X86_REG_INVALID)
                && (x86->operands[1].mem.base == x86->operands[0].reg)
                && (x86->operands[1].mem.index == X86_REG_INVALID)
                /* mem.scale is irrelevant since index is not used */
                && (x86->operands[1].mem.disp == 0)) {
                return true;
            }
            /* lea        reg,[reg + eiz*x + 0x0] */
            if((x86->op_count == 2)
                && (x86->operands[0].type == X86_OP_REG)
                && (x86->operands[1].type == X86_OP_MEM)
                && (x86->operands[1].mem.segment == X86_REG_INVALID)
                && (x86->operands[1].mem.base == x86->operands[0].reg)
                && (x86->operands[1].mem.index == X86_REG_EIZ)
                /* mem.scale is irrelevant since index is the zero-register */
                && (x86->operands[1].mem.disp == 0)) {
                return true;
            }
            return false;
        default:
            return false;
    }
}

bool Disassembler::IsTrapInsn(cs_insn *ins){
    switch(ins->id) {
        case X86_INS_INT3:
        case X86_INS_UD2:
        case X86_INS_INT1:
        case X86_INS_INTO:
            return true;
        default:
            return false;
    }
}

bool Disassembler::IsSuspiciousInsn(cs_insn *insn){
    return (IsPaddingInsn(insn) || IsSemanticNopInsn(insn) || IsTrapInsn(insn) || IsPrivInsn(insn));
}

bool Disassembler::IsPrivInsn(cs_insn *ins){
    switch(ins->id) {
        case X86_INS_HLT:
        case X86_INS_IN:
        case X86_INS_INSB:
        case X86_INS_INSW:
        case X86_INS_INSD:
        case X86_INS_OUT:
        case X86_INS_OUTSB:
        case X86_INS_OUTSW:
        case X86_INS_OUTSD:
        case X86_INS_RDMSR:
        case X86_INS_WRMSR:
        case X86_INS_RDPMC:
        case X86_INS_RDTSC:
        case X86_INS_LGDT:
        case X86_INS_LLDT:
        case X86_INS_LTR:
        case X86_INS_LMSW:
        case X86_INS_CLTS:
        case X86_INS_INVD:
        case X86_INS_INVLPG:
        case X86_INS_WBINVD:
            return true;
        default:
            return false;
    }
}

bool Disassembler::IsWildcardInsn(cs_insn *insn){
    switch (insn->id) {
        case X86_INS_NOP:
        case X86_INS_FNOP:
            return true;
        default:
            break;
    }
    return false;
}

bool Disassembler::IsRetInsn(cs_insn *insn) {
    switch (insn->id) {
        case X86_INS_RET:
        case X86_INS_RETF:
        case X86_INS_RETFQ:
        case X86_INS_IRET:
        case X86_INS_IRETD:
        case X86_INS_IRETQ:
            return true;
        default:
            return false;
    }
}

bool Disassembler::IsJumpInsn(cs_insn *insn){
    if (IsUnconditionalJumpInsn(insn) == true){
        return true;
    }
    if (IsConditionalJumpInsn(insn) == true){
        return true;
    }
    return false;
}

uint64_t Disassembler::GetInsnEdges(cs_insn *insn){
    if (IsUnconditionalJumpInsn(insn) == true){
        return 1;
    }
    if (IsConditionalJumpInsn(insn) == true){
        return 2;
    }
    return 0;
}

bool Disassembler::IsUnconditionalJumpInsn(cs_insn *insn){
    switch (insn->id) {
        case X86_INS_JMP:
            return true;
        default:
            return false;
    }
}

bool Disassembler::IsConditionalJumpInsn(cs_insn* insn) {
    switch (insn->id) {
        case X86_INS_JNE:
        case X86_INS_JNO:
        case X86_INS_JNP:
        case X86_INS_JL:
        case X86_INS_JLE:
        case X86_INS_JG:
        case X86_INS_JGE:
        case X86_INS_JE:
        case X86_INS_JECXZ:
        case X86_INS_JCXZ:
        case X86_INS_JB:
        case X86_INS_JBE:
        case X86_INS_JA:
        case X86_INS_JAE:
        case X86_INS_JNS:
        case X86_INS_JO:
        case X86_INS_JP:
        case X86_INS_JRCXZ:
        case X86_INS_JS:
        case X86_INS_LOOPE:
        case X86_INS_LOOPNE:
            return true;
        default:
            return false;
    }
}

bool Disassembler::IsCallInsn(cs_insn *insn){
    switch(insn->id){
        case X86_INS_CALL:
        case X86_INS_LCALL:
            return true;
        default:
            break;
    }
    return false;
}

DISASSEMBLER_OPERAND_TYPE Disassembler::CollectInsn(cs_insn* insn, struct Section *sections, uint index) {
    DISASSEMBLER_OPERAND_TYPE result = DISASSEMBLER_OPERAND_TYPE_UNSET;
    if (IsJumpInsn(insn) == true){
        CollectOperands(insn, DISASSEMBLER_OPERAND_TYPE_BLOCK, sections, index);
        result = DISASSEMBLER_OPERAND_TYPE_BLOCK;
        return result;
    }
    if (IsCallInsn(insn) == true){
        CollectOperands(insn, DISASSEMBLER_OPERAND_TYPE_FUNCTION, sections, index);
        result = DISASSEMBLER_OPERAND_TYPE_FUNCTION;
        return result;
    }
    return result;
}

void Disassembler::AddDiscoveredBlock(uint64_t address, struct Section *sections, uint index) {
    if (IsVisited(sections[index].visited, address) == false &&
    address < sections[index].data_size &&
    IsFunction(sections[index].functions, address) == false) {
        if (sections[index].blocks.insert(address).second == true){
            sections[index].visited[address] = DISASSEMBLER_VISITED_QUEUED;
            sections[index].discovered.push(address);
        }
    }
}

void Disassembler::AddDiscoveredFunction(uint64_t address, struct Section *sections, uint index) {
    if (IsVisited(sections[index].visited, address) == false && address < sections[index].data_size) {
        if (sections[index].functions.insert(address).second == true){
            sections[index].visited[address] = DISASSEMBLER_VISITED_QUEUED;
            sections[index].discovered.push(address);
        }
    }
}

void Disassembler::CollectOperands(cs_insn* insn, int operand_type, struct Section *sections, uint index) {
    uint64_t address = X86_REL_ADDR(*insn);
    switch(operand_type){
        case DISASSEMBLER_OPERAND_TYPE_BLOCK:
            {
                AddDiscoveredBlock(address, sections, index);
                AddDiscoveredBlock(insn->address + insn->size, sections, index);
                break;
            }
        case DISASSEMBLER_OPERAND_TYPE_FUNCTION:
            {
                AddDiscoveredFunction(address, sections, index);
                break;
            }
        default:
            break;
    }
}

bool Disassembler::IsFunction(set<uint64_t> &addresses, uint64_t address){
    if (addresses.find(address) != addresses.end()){
        return true;
    }
    return false;
}

bool Disassembler::IsBlock(set<uint64_t> &addresses, uint64_t address){
    if (addresses.find(address) != addresses.end()){
        return true;
    }
    return false;
}

Disassembler::~Disassembler() {}
