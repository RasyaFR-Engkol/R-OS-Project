#pragma once

#define CATCHARG1(ctx) ctx->rdi
#define CATCHARG2(ctx) ctx->rsi
#define CATCHARG3(ctx) ctx->rdx
#define CATCHARG4(ctx) ctx->r10
#define CATCHARG5(ctx) ctx->r9
#define CATCHARG6(ctx) ctx->r8
#define RETVAL(ctx) ctx->rax