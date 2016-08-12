/*
** ARM64 instruction emitter.
** Copyright !!!TODO
*/

static Reg ra_allock(ASMState *as, int32_t k, RegSet allow);

static void emit_loadn(ASMState *as, Reg r, cTValue *tv)
{
  lua_unimpl();
}

/* !!!TODO non-ARM ports are different */
#define emit_canremat(ref)      ((ref) < ASMREF_L)

#define glofs(as, k) \
  ((intptr_t)((uintptr_t)(k) - (uintptr_t)&J2GG(as->J)->g))

#define emit_getgl(as, r, field) \
  emit_lsptr(as, A64I_LDRx, (r), (void *)&J2G(as->J)->field)

static void emit_d(ASMState *as, A64Ins ai, Reg rd)
{
  *--as->mcp = ai | A64F_D(rd);
}

static void emit_n(ASMState *as, A64Ins ai, Reg rn)
{
  *--as->mcp = ai | A64F_N(rn);
}

static void emit_dn(ASMState *as, A64Ins ai, Reg rd, Reg rn)
{
  *--as->mcp = ai | A64F_D(rd) | A64F_N(rn);
}

static void emit_dm(ASMState *as, A64Ins ai, Reg rd, Reg rm)
{
  *--as->mcp = ai | A64F_D(rd) | A64F_M(rm);
}

static void emit_nm(ASMState *as, A64Ins ai, Reg rn, Reg rm)
{
  *--as->mcp = ai | A64F_N(rn) | A64F_M(rm);
}

static void emit_dnm(ASMState *as, A64Ins ai, Reg rd, Reg rn, Reg rm)
{
  *--as->mcp = ai | A64F_D(rd) | A64F_N(rn) | A64F_M(rm);
}

/* Encode constant in K12 format for data processing instructions. */
static uint32_t emit_isk12(A64Ins ai, int32_t n)
{
    if (n >= 0 && n <= 4095)
    {
        return (n & 4095) << 10;
    }
    if ((n & 4095) == 0 && n < (4095 << 12))
    {
        return (((n >> 12) & 4095) << 10) | (1 << 22);
    }
    return -1;
}

/* Encode constant in K13 format for data processing instructions. */
static uint32_t emit_isk13(A64Ins ai, int64_t n) {
  lua_unimpl();
}

/* -- Emit loads/stores --------------------------------------------------- */

typedef enum {
  OFS_INVALID,
  OFS_UNSCALED,
  OFS_SCALED_0,
  OFS_SCALED_1,
  OFS_SCALED_2,
  OFS_SCALED_3,
} ofs_type;

static ofs_type check_offset(A64Ins ai, int32_t ofs)
{
  int scale;
  switch (ai)
  {
  case A64I_LDRBw: scale = 0; break;
  case A64I_STRBw: scale = 0; break;
  case A64I_LDRHw: scale = 1; break;
  case A64I_STRHw: scale = 1; break;
  case A64I_LDRw: scale = 2; break;
  case A64I_STRw: scale = 2; break;
  case A64I_LDRs: scale = 2; break;
  case A64I_STRs: scale = 2; break;
  case A64I_LDRx: scale = 3; break;
  case A64I_STRx: scale = 3; break;
  case A64I_LDRd: scale = 3; break;
  case A64I_STRd: scale = 3; break;
  default: lua_assert(!"invalid instruction in check_offset");
  }

  /* do we need to use unscaled op? */
  if (ofs < 0 || (ofs & ((1<<scale)-1)))
  {
    /* unaligned, so need to use u variant (eg ldur) */
    return (ofs >= -256 && ofs <= 255) ? OFS_UNSCALED : OFS_INVALID;
  } else {
    return (ofs >= 0 && ofs <= (4096<<scale)) ? OFS_SCALED_0 + scale : OFS_INVALID;
  }
}

static void emit_lso(ASMState *as, A64Ins ai, Reg rd, Reg rn, int32_t ofs)
{
  /* !!!TODO ARM emit_lso combines LDR/STR pairs into LDRD/STRD, something
     similar possible here? */
  ofs_type ot = check_offset(ai, ofs);
  lua_assert(ot != OFS_INVALID);
  if (ot == OFS_UNSCALED) {
    ai ^= A64I_LS_U;
    *--as->mcp = ai | A64F_D(rd) | A64F_N(rn) | A64F_A_U(ofs & 0x1ff);
  } else {
    int32_t ofs_field;
    ofs_field = ofs >> (ot - OFS_SCALED_0);
    *--as->mcp = ai | A64F_D(rd) | A64F_N(rn) | A64F_A(ofs_field);
  }
}

static void emit_lsptr(ASMState *as, A64Ins ai, Reg r, void *p)
{
  int64_t ofs = glofs(as, p);
  if (checki32(ofs) && check_offset(ai, ofs)) {
    emit_lso(as, ai, r, RID_GL, ofs);
  } else {
    int64_t i = i64ptr(p);
    Reg tmp = RID_TMP; /*!!!TODO allocate register? */
    emit_lso(as, ai, r, tmp, 0);
    *--as->mcp = A64I_MOVK_48x | A64F_D(tmp) | A64F_U16((i>>48) & 0xffff);
    *--as->mcp = A64I_MOVK_32x | A64F_D(tmp) | A64F_U16((i>>32) & 0xffff);
    *--as->mcp = A64I_MOVK_16x | A64F_D(tmp) | A64F_U16((i>>16) & 0xffff);
    *--as->mcp = A64I_MOVZx | A64F_D(tmp) | A64F_U16(i & 0xffff);
  }
}

/* Load a 32 bit constant into a GPR. */
static void emit_loadi(ASMState *as, Reg rd, int32_t i)
{
  /* !!!TODO handle wide move */
  if (i & 0xffff0000) {
    *--as->mcp = A64I_MOVK_16w | A64F_D(rd) | A64F_U16((i>>16) & 0xffff);
  }
  *--as->mcp = A64I_MOVZw | A64F_D(rd) | A64F_U16(i & 0xffff);
}

/* mov r, imm64 or shorter 32 bit extended load. */
static void emit_loadu64(ASMState *as, Reg rd, uint64_t u64)
{
  /* !!!TODO plenty of ways to optimise this! */
  if (u64 & 0xffff000000000000) {
    *--as->mcp = A64I_MOVK_48x | A64F_D(rd) | A64F_U16((u64>>48) & 0xffff);
  }
  if (u64 & 0xffff00000000) {
    *--as->mcp = A64I_MOVK_32x | A64F_D(rd) | A64F_U16((u64>>32) & 0xffff);
  }
  if (u64 & 0xffff0000) {
    *--as->mcp = A64I_MOVK_16x | A64F_D(rd) | A64F_U16((u64>>16) & 0xffff);
  }
  *--as->mcp = A64I_MOVZw | A64F_D(rd) | A64F_U16(u64 & 0xffff);
}

/* Generic load of register with base and (small) offset address. */
static void emit_loadofs(ASMState *as, IRIns *ir, Reg r, Reg base, int32_t ofs)
{
#if LJ_SOFTFP
  lua_assert(!irt_isnum(ir->t)); UNUSED(ir);
#else
  if (r >= RID_MAX_GPR)
    emit_lso(as, irt_isnum(ir->t) ? A64I_LDRd : A64I_LDRs, r, base, ofs);
  else
#endif
    emit_lso(as, A64I_LDRx, r, base, ofs);
}

/* Generic store of register with base and (small) offset address. */
static void emit_storeofs(ASMState *as, IRIns *ir, Reg r, Reg base, int32_t ofs)
{
  if (r >= RID_MAX_GPR) {
    emit_lso(as, irt_isnum(ir->t) ? A64I_STRd : A64I_STRs, r, base, ofs);
  } else {
    emit_lso(as, A64I_STRx, r, base, ofs);
  }
}

/* Generic move between two regs. */
static void emit_movrr(ASMState *as, IRIns *ir, Reg dst, Reg src)
{
#if LJ_SOFTFP
  lua_assert(!irt_isnum(ir->t)); UNUSED(ir);
#else
  if (dst >= RID_MAX_GPR) {
    emit_dn(as, irt_isnum(ir->t) ? A64I_FMOV_D : A64I_FMOV_S,
     (dst & 31), (src & 31));
    return;
  }
#endif

// TODO: add swapping early registers for loads/stores?

  emit_dm(as, A64I_MOVx, dst, src);
}

/* Emit an arithmetic/logic operation with a constant operand. */
static void emit_opk(ASMState *as, A64Ins ai, Reg dest, Reg src,
                     int32_t i, RegSet allow)
{
  uint32_t k = emit_isk12(ai, i);
  if (k != -1)
    emit_dn(as, ai^k^A64I_BINOPk, dest, src);
  else
    emit_dnm(as, ai, dest, src, ra_allock(as, i, allow));
}

static void emit_ccmpr(ASMState *as, A64Ins ai, A64CC cond, int32_t nzcv, Reg
rn, Reg rm)
{
  *--as->mcp = ai | A64F_N(rn) | A64F_M(rm) | A64F_NZCV(nzcv) | A64F_COND(cond);
}

static void emit_ccmpk(ASMState *as, A64Ins ai, A64CC cond, int32_t nzcv, Reg
rn, int32_t k, RegSet allow)
{
  if (k >=0 && k <= 31)
    *--as->mcp =
      ai | A64F_N (rn) | A64F_M (k) | A64F_NZCV (nzcv) | A64F_COND (cond);
  else
  {
    emit_ccmpr(as, ai, cond, nzcv, rn, ra_allock(as, k, allow));
  }
}

static Reg ra_scratch(ASMState *as, RegSet allow);

/* Load 64 bit IR constant into register. */
static void emit_loadk64(ASMState *as, Reg r, IRIns *ir)
{
  // TODO: This can probably be optimized.
  Reg r64 = r;
  uint64_t k = ir_k64(ir)->u64;
  if (rset_test(RSET_FPR, r)) {
    r64 = ra_scratch(as, RSET_GPR);
    emit_dn(as, A64I_FMOV_D_R, (r & 31), r64);
  }
  emit_loadu64(as, r64, k);
}

/* -- Emit control-flow instructions -------------------------------------- */

/* Label for internal jumps. */
typedef MCode *MCLabel;

/* Return label pointing to current PC. */
#define emit_label(as)		((as)->mcp)

static void emit_branch(ASMState *as, A64Ins ai, MCode *target)
{
  MCode *p = as->mcp;
  ptrdiff_t delta = target - (p - 1);
  lua_assert(((delta + 0x02000000) >> 26) == 0);
  *--p = ai | ((uint32_t)delta & 0x03ffffffu);
  as->mcp = p;
}

static void emit_cond_branch(ASMState *as, A64CC cond, MCode *target)
{
  MCode *p = as->mcp;
  ptrdiff_t delta = target - (p - 1);
  lua_assert(((delta + 0x40000) >> 19) == 0);
  *--p = A64I_Bcond | (((uint32_t)delta & 0x7ffff)<<5) | cond;
  as->mcp = p;
}

/* Add offset to pointer. */
static void emit_addptr(ASMState *as, Reg r, int32_t ofs)
{
  if (ofs)
  {
    A64Ins op = ofs < 0 ? A64I_SUBx : A64I_ADDx;
    if (ofs < 0)
      ofs = -ofs;
    emit_opk(as, op, r, r, ofs, rset_exclude(RSET_GPR, r));
  }
}

#define emit_jmp(as, target) lua_unimpl()

#define emit_setvmstate(as, i)                UNUSED(i)
#define emit_spsub(as, ofs)                   emit_addptr(as, RID_SP, -(ofs))
#define emit_setgl(as, r, field)              lua_unimpl()

static void emit_call(ASMState *as, void *target)
{
  static const int32_t imm_bits = 26;
  MCode *p = --as->mcp;
  ptrdiff_t delta = (char *)target - (char *)p;
  delta >>= 2;
  if ((delta + (1 << (imm_bits -1))) >= 0) {
    *p = A64I_BL | (delta & ((1 << imm_bits) - 1));
  } else {  /* Target out of range. */
    lua_unimpl();
  }
}
