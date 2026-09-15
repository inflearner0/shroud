#include "shroud/VMBytecode.h"

#include <stdint.h>
#include <string.h>

typedef uint64_t (*ShroudVMFn8)(uint64_t, uint64_t, uint64_t, uint64_t,
                                uint64_t, uint64_t, uint64_t, uint64_t);

static uint64_t shroud_vm_splitmix(uint64_t *state) {
  uint64_t z = (*state += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

static uint64_t shroud_vm_mix(uint64_t x) {
  x += 0x9E3779B97F4A7C15ULL;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
  return x ^ (x >> 31);
}

void shroud_vm_decrypt(const uint64_t *in, uint64_t *out, uint64_t count,
                       uint64_t key) {
  uint64_t state = key;
  for (uint64_t i = 0; i < count; ++i)
    out[i] = in[i] ^ shroud_vm_splitmix(&state);
}

void shroud_vm_decrypt_blocks(const uint64_t *in, uint64_t *out, uint64_t count,
                              uint64_t key, const uint64_t *starts,
                              uint64_t nstarts) {
  uint64_t state = key;
  uint64_t next = 0;
  for (uint64_t pc = 0; pc < count; ++pc) {
    if (next < nstarts && pc == starts[next]) {
      if (pc != 0) state = shroud_vm_mix(state ^ 0x5DEECE66DULL);
      ++next;
    }
    state = shroud_vm_mix(state);
    out[pc] = in[pc] ^ state;
  }
}

void shroud_vm_trap(void) { __builtin_trap(); }

static uint64_t shroud_vm_rb(const uint64_t *regs, unsigned b, int64_t imm) {
  return b == SHROUD_VM_REG_SENTINEL ? (uint64_t)imm : regs[b];
}

uint64_t shroud_vm_run(const uint64_t *code, uint64_t cells, uint64_t *regs,
                       void *const *call_table, uint64_t call_count,
                       void *const *global_table, uint64_t global_count,
                       const uint64_t *verify_src, uint64_t verify_sum) {
  (void)call_count;
  (void)global_count;
  uint64_t pc = 0;
  uint64_t guard = 0;
  if (!verify_src) verify_sum = 0;
  for (;;) {
    if (pc >= cells) return regs[0];
    if (verify_src && (++guard & 0xFFF) == 0) {
      uint64_t h = 0xCBF29CE484222325ULL;
      for (uint64_t i = 0; i < cells; ++i) {
        h ^= verify_src[i];
        h *= 0x100000001B3ULL;
      }
      if (h != verify_sum) __builtin_trap();
    }
    uint64_t insn = code[pc++];
    unsigned op = (unsigned)(insn & 0xFF);
    unsigned dst = (unsigned)((insn >> 8) & 0xFF);
    unsigned a = (unsigned)((insn >> 16) & 0xFF);
    unsigned b = (unsigned)((insn >> 24) & 0xFF);
    int64_t imm = (int64_t)(int32_t)(uint32_t)(insn >> 32);
    switch (op) {
    case SHROUD_VM_HALT:
      return regs[0];
    case SHROUD_VM_RET:
      return a == SHROUD_VM_REG_SENTINEL ? 0 : regs[a];
    case SHROUD_VM_CONST:
      regs[dst] = (uint64_t)(uint32_t)imm;
      break;
    case SHROUD_VM_CONST64: {
      uint64_t hi = (uint64_t)(uint32_t)(code[pc++] >> 32);
      regs[dst] = ((uint64_t)(uint32_t)imm) | (hi << 32);
      break;
    }
    case SHROUD_VM_MOV:
      regs[dst] = regs[a];
      break;
    case SHROUD_VM_MOV_ALT:
      regs[dst] = (regs[a] | (regs[a] & 0)) ^ (regs[a] & 0);
      break;
    case SHROUD_VM_ADD:
      regs[dst] = regs[a] + shroud_vm_rb(regs, b, imm);
      break;
    case SHROUD_VM_ADD_ALT: {
      uint64_t x = regs[a];
      uint64_t y = shroud_vm_rb(regs, b, imm);
      regs[dst] = (x ^ y) + 2 * (x & y);
      break;
    }
    case SHROUD_VM_SUB:
      regs[dst] = regs[a] - shroud_vm_rb(regs, b, imm);
      break;
    case SHROUD_VM_MUL:
      regs[dst] = regs[a] * shroud_vm_rb(regs, b, imm);
      break;
    case SHROUD_VM_UDIV: {
      uint64_t d = shroud_vm_rb(regs, b, imm);
      regs[dst] = d ? regs[a] / d : 0;
      break;
    }
    case SHROUD_VM_SDIV: {
      int64_t x = (int64_t)regs[a];
      int64_t y = (int64_t)shroud_vm_rb(regs, b, imm);
      uint64_t r = 0;
      if (y == -1)
        r = (uint64_t)(0 - (uint64_t)x);
      else if (y != 0)
        r = (uint64_t)(x / y);
      regs[dst] = r;
      break;
    }
    case SHROUD_VM_UREM: {
      uint64_t d = shroud_vm_rb(regs, b, imm);
      regs[dst] = d ? regs[a] % d : 0;
      break;
    }
    case SHROUD_VM_SREM: {
      int64_t x = (int64_t)regs[a];
      int64_t y = (int64_t)shroud_vm_rb(regs, b, imm);
      int64_t r = 0;
      if (y == -1)
        r = 0;
      else if (y != 0)
        r = x % y;
      regs[dst] = (uint64_t)r;
      break;
    }
    case SHROUD_VM_AND:
      regs[dst] = regs[a] & shroud_vm_rb(regs, b, imm);
      break;
    case SHROUD_VM_OR:
      regs[dst] = regs[a] | shroud_vm_rb(regs, b, imm);
      break;
    case SHROUD_VM_XOR:
      regs[dst] = regs[a] ^ shroud_vm_rb(regs, b, imm);
      break;
    case SHROUD_VM_XOR_ALT: {
      uint64_t x = regs[a];
      uint64_t y = shroud_vm_rb(regs, b, imm);
      regs[dst] = (x | y) - (x & y);
      break;
    }
    case SHROUD_VM_SHL:
      regs[dst] = regs[a] << (shroud_vm_rb(regs, b, imm) & 63);
      break;
    case SHROUD_VM_LSHR:
      regs[dst] = regs[a] >> (shroud_vm_rb(regs, b, imm) & 63);
      break;
    case SHROUD_VM_ASHR:
      regs[dst] = (uint64_t)((int64_t)regs[a] >> (shroud_vm_rb(regs, b, imm) & 63));
      break;
    case SHROUD_VM_NEG:
      regs[dst] = 0 - regs[a];
      break;
    case SHROUD_VM_NOT:
      regs[dst] = ~regs[a];
      break;
    case SHROUD_VM_ICMP_EQ:
      regs[dst] = regs[a] == shroud_vm_rb(regs, b, imm) ? 1 : 0;
      break;
    case SHROUD_VM_ICMP_EQ_ALT:
      regs[dst] = (regs[a] ^ shroud_vm_rb(regs, b, imm)) == 0 ? 1 : 0;
      break;
    case SHROUD_VM_ICMP_NE:
      regs[dst] = regs[a] != shroud_vm_rb(regs, b, imm) ? 1 : 0;
      break;
    case SHROUD_VM_ICMP_ULT:
      regs[dst] = regs[a] < shroud_vm_rb(regs, b, imm) ? 1 : 0;
      break;
    case SHROUD_VM_ICMP_ULE:
      regs[dst] = regs[a] <= shroud_vm_rb(regs, b, imm) ? 1 : 0;
      break;
    case SHROUD_VM_ICMP_UGT:
      regs[dst] = regs[a] > shroud_vm_rb(regs, b, imm) ? 1 : 0;
      break;
    case SHROUD_VM_ICMP_UGE:
      regs[dst] = regs[a] >= shroud_vm_rb(regs, b, imm) ? 1 : 0;
      break;
    case SHROUD_VM_ICMP_SLT:
      regs[dst] = (int64_t)regs[a] < (int64_t)shroud_vm_rb(regs, b, imm) ? 1 : 0;
      break;
    case SHROUD_VM_ICMP_SLE:
      regs[dst] = (int64_t)regs[a] <= (int64_t)shroud_vm_rb(regs, b, imm) ? 1 : 0;
      break;
    case SHROUD_VM_ICMP_SGT:
      regs[dst] = (int64_t)regs[a] > (int64_t)shroud_vm_rb(regs, b, imm) ? 1 : 0;
      break;
    case SHROUD_VM_ICMP_SGE:
      regs[dst] = (int64_t)regs[a] >= (int64_t)shroud_vm_rb(regs, b, imm) ? 1 : 0;
      break;
    case SHROUD_VM_SELECT:
      regs[dst] = regs[(unsigned)imm] ? regs[a] : regs[b];
      break;
    case SHROUD_VM_JMP:
      pc = (uint64_t)(uint32_t)imm;
      break;
    case SHROUD_VM_CONDBR:
      if (regs[a]) pc = (uint64_t)(uint32_t)imm;
      break;
    case SHROUD_VM_SEXT: {
      uint64_t w = (uint64_t)imm;
      regs[dst] = w >= 64 ? regs[a]
                          : (uint64_t)(((int64_t)(regs[a] << (64 - w))) >> (64 - w));
      break;
    }
    case SHROUD_VM_TRUNC: {
      uint64_t w = (uint64_t)imm;
      regs[dst] = w >= 64 ? regs[a] : (regs[a] & (((uint64_t)1 << w) - 1));
      break;
    }
    case SHROUD_VM_LOAD8:
      regs[dst] = *(const uint8_t *)(uintptr_t)regs[a];
      break;
    case SHROUD_VM_LOAD16:
      regs[dst] = *(const uint16_t *)(uintptr_t)regs[a];
      break;
    case SHROUD_VM_LOAD32:
      regs[dst] = *(const uint32_t *)(uintptr_t)regs[a];
      break;
    case SHROUD_VM_LOAD32_ALT:
      regs[dst] = *(const volatile uint32_t *)(uintptr_t)regs[a];
      break;
    case SHROUD_VM_LOAD64:
      regs[dst] = *(const uint64_t *)(uintptr_t)regs[a];
      break;
    case SHROUD_VM_STORE8:
      *(uint8_t *)(uintptr_t)regs[a] = (uint8_t)regs[b];
      break;
    case SHROUD_VM_STORE16:
      *(uint16_t *)(uintptr_t)regs[a] = (uint16_t)regs[b];
      break;
    case SHROUD_VM_STORE32:
      *(uint32_t *)(uintptr_t)regs[a] = (uint32_t)regs[b];
      break;
    case SHROUD_VM_STORE64:
      *(uint64_t *)(uintptr_t)regs[a] = regs[b];
      break;
    case SHROUD_VM_GEP_IMM:
      regs[dst] = regs[a] + (uint64_t)imm;
      break;
    case SHROUD_VM_GEP_REG:
      regs[dst] = regs[a] + regs[b] * (uint64_t)imm;
      break;
    case SHROUD_VM_GEP_LOAD32:
      regs[dst] = *(const uint32_t *)(uintptr_t)(regs[a] + regs[b] * (uint64_t)imm);
      break;
    case SHROUD_VM_GEP_LOAD64:
      regs[dst] = *(const uint64_t *)(uintptr_t)(regs[a] + regs[b] * (uint64_t)imm);
      break;
    case SHROUD_VM_GEP_STORE32:
      *(uint32_t *)(uintptr_t)(regs[a] + regs[b] * (uint64_t)imm) = (uint32_t)regs[dst];
      break;
    case SHROUD_VM_GEP_STORE64:
      *(uint64_t *)(uintptr_t)(regs[a] + regs[b] * (uint64_t)imm) = regs[dst];
      break;
    case SHROUD_VM_CALL: {
      ShroudVMFn8 fn = (ShroudVMFn8)call_table[(uint32_t)imm];
      regs[dst] = fn(regs[SHROUD_VM_ARG_BASE + 0], regs[SHROUD_VM_ARG_BASE + 1],
                     regs[SHROUD_VM_ARG_BASE + 2], regs[SHROUD_VM_ARG_BASE + 3],
                     regs[SHROUD_VM_ARG_BASE + 4], regs[SHROUD_VM_ARG_BASE + 5],
                     regs[SHROUD_VM_ARG_BASE + 6], regs[SHROUD_VM_ARG_BASE + 7]);
      break;
    }
    case SHROUD_VM_CALL_INDIRECT: {
      ShroudVMFn8 fn = (ShroudVMFn8)regs[a];
      regs[dst] = fn(regs[SHROUD_VM_ARG_BASE + 0], regs[SHROUD_VM_ARG_BASE + 1],
                     regs[SHROUD_VM_ARG_BASE + 2], regs[SHROUD_VM_ARG_BASE + 3],
                     regs[SHROUD_VM_ARG_BASE + 4], regs[SHROUD_VM_ARG_BASE + 5],
                     regs[SHROUD_VM_ARG_BASE + 6], regs[SHROUD_VM_ARG_BASE + 7]);
      break;
    }
    case SHROUD_VM_LOADADDR:
      regs[dst] = (uint64_t)(uintptr_t)global_table[(uint32_t)imm];
      break;
    case SHROUD_VM_MEMCPY:
      memcpy((void *)(uintptr_t)regs[a], (const void *)(uintptr_t)regs[b],
             (size_t)regs[(unsigned)imm]);
      break;
    case SHROUD_VM_MEMMOVE:
      memmove((void *)(uintptr_t)regs[a], (const void *)(uintptr_t)regs[b],
              (size_t)regs[(unsigned)imm]);
      break;
    case SHROUD_VM_MEMSET:
      memset((void *)(uintptr_t)regs[a], (int)regs[b], (size_t)regs[(unsigned)imm]);
      break;
    case SHROUD_VM_BSWAP16:
      regs[dst] = (uint64_t)__builtin_bswap16((uint16_t)regs[a]);
      break;
    case SHROUD_VM_BSWAP32:
      regs[dst] = (uint64_t)__builtin_bswap32((uint32_t)regs[a]);
      break;
    case SHROUD_VM_BSWAP64:
      regs[dst] = __builtin_bswap64(regs[a]);
      break;
    default:
      return regs[0];
    }
  }
}
