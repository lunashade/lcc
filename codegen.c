#include "lcc.h"

//
// Codegen
//

// register
static char *argreg64[] = {"rdi", "rsi", "rdx", "rcx", "r8", "r9"};
static char *argreg32[] = {"edi", "esi", "edx", "ecx", "r8d", "r9d"};
static char *argreg8[] = {"dil", "sil", "dl", "cl", "r8b", "r9b"};

static char *reg(int idx) {
    static char *r[] = {"r10", "r11", "r12", "r13", "r14", "r15"};

    if (idx < 0 || sizeof(r) / sizeof(*r) <= idx)
        error("registor out of range: %d", idx);
    return r[idx];
}

static int top;
static char *reg_push() { return reg(top++); }
static char *reg_pop() { return reg(--top); }

// label
static int labelcnt;
static int next_label() { return ++labelcnt; }

static char *funcname; // currently generating

// address

// load a value from stack top is pointing to.
static void load(Type *ty) {
    if (ty->kind == TY_ARRAY || ty->kind == TY_STRUCT) {
        // do nothing since generally array or struct can't be loaded to a
        // register. As a result, evaluation is converted to pointer to the
        // first element.
        return;
    }
    char *rd = reg_pop();
    if (ty->size == 1) {
        printf("\tmovsx %s, byte ptr [%s]\n", reg_push(), rd);

    } else {
        assert(ty->size == 8);
        printf("\tmov %s, [%s]\n", reg_push(), rd);
    }
    return;
}
// pop address and value, store value into address, push address
static void store(Type *ty) {
    char *rd = reg_pop();
    char *rs = reg_pop();
    if (ty->kind == TY_STRUCT) {
        // TODO: if size<8, load on reg
        // TODO: use alignment
        for (int i = 0; i < ty->size; i++) {
            printf("\tmov al, [%s+%d]\n", rs, i);
            printf("\tmov [%s+%d], al\n", rd, i);
        }
    } else if (ty->size == 1) {
        printf("\tmov [%s], %sb\n", rd, rs);
    } else {
        assert(ty->size == 8);
        printf("\tmov [%s], %s\n", rd, rs);
    }
    reg_push(); // address to top
    return;
}

// code generation
static void gen_addr(Node *node);
static void gen_expr(Node *node);
static void gen_stmt(Node *node);

// code generate address of node
static void gen_addr(Node *node) {
    if (node->kind == ND_VAR) {
        if (node->var->is_local)
            printf("\tlea %s, [rbp-%d]\n", reg_push(), node->var->offset);
        else
            printf("\tmov %s, offset %s\n", reg_push(), node->var->name);

        return;
    }
    if (node->kind == ND_DEREF) {
        gen_expr(node->lhs);
        return;
    }
    if (node->kind == ND_MEMBER) {
        gen_addr(node->lhs);
        printf("\tadd %s, %d\n", reg_pop(), node->member->offset);
        reg_push();
        return;
    }
    error_tok(node->tok, "codegen: gen_addr: not an lvalue");
}

// code generate expression
static void gen_expr(Node *node) {
    printf(".loc 1 %d\n", node->tok->lineno);
    switch (node->kind) {
    case ND_NUM:
        printf("\tmov %s, %ld\n", reg_push(), node->val);
        return;
    case ND_VAR:
    case ND_MEMBER:
        gen_addr(node);
        load(node->ty);
        return;
    case ND_ADDR:
        gen_addr(node->lhs);
        return;
    case ND_DEREF:
        gen_expr(node->lhs);
        load(node->ty);
        return;
    case ND_ASSIGN:
        if (node->ty->kind == TY_ARRAY) {
            error_tok(node->tok, "codegen: assign: array is not an lvalue");
        }
        gen_expr(node->rhs);
        gen_addr(node->lhs);
        store(node->ty);
        return;
    case ND_STMT_EXPR:
        for (Node *n = node->body; n; n = n->next) {
            gen_stmt(n);
        }
        reg_push();
        return;
    case ND_FUNCALL: {
        int top_orig = top;
        top = 0;
        printf("\tpush r10\n");
        printf("\tpush r11\n");
        printf("\tpush r12\n");
        printf("\tpush r13\n");
        printf("\tpush r14\n");
        printf("\tpush r15\n");

        // push arguments then pop to register
        int nargs = 0;
        for (Node *arg = node->args; arg; arg = arg->next) {
            if (nargs >= 6) {
                error_tok(arg->tok, "codegen: funcall: too many arguments: %d",
                          nargs);
            }
            gen_expr(arg);
            printf("\tpush %s\n", reg_pop());
            printf("\tsub rsp, 8\n");
            nargs++;
        }
        for (int i = nargs - 1; i >= 0; i--) {
            printf("\tadd rsp, 8\n");
            printf("\tpop %s\n", argreg64[i]);
        }

        printf("\tmov rax, 0\n");
        printf("\tcall %s\n", node->funcname);

        top = top_orig;
        printf("\tpop r15\n");
        printf("\tpop r14\n");
        printf("\tpop r13\n");
        printf("\tpop r12\n");
        printf("\tpop r11\n");
        printf("\tpop r10\n");

        printf("\tmov %s, rax\n", reg_push());
        return;
    }
    }

    // binary node
    gen_expr(node->lhs);
    gen_expr(node->rhs);

    char *rs = reg_pop();
    char *rd = reg_pop();
    reg_push();

    if (node->kind == ND_ADD) {
        printf("\tadd %s, %s\n", rd, rs);
        return;
    }
    if (node->kind == ND_SUB) {
        printf("\tsub %s, %s\n", rd, rs);
        return;
    }
    if (node->kind == ND_MUL) {
        printf("\timul %s, %s\n", rd, rs);
        return;
    }
    if (node->kind == ND_DIV) {
        printf("\tmov rax, %s\n", rd);
        printf("\tcqo\n");
        printf("\tidiv %s\n", rs);
        printf("\tmov %s, rax\n", rd);
        return;
    }
    if (node->kind == ND_EQ) {
        printf("\tcmp %s, %s\n", rd, rs);
        printf("\tsete al\n");
        printf("\tmovzb rax, al\n");
        printf("\tmov %s, rax\n", rd);
        return;
    }
    if (node->kind == ND_NE) {
        printf("\tcmp %s, %s\n", rd, rs);
        printf("\tsetne al\n");
        printf("\tmovzb rax, al\n");
        printf("\tmov %s, rax\n", rd);
        return;
    }
    if (node->kind == ND_LT) {
        printf("\tcmp %s, %s\n", rd, rs);
        printf("\tsetl al\n");
        printf("\tmovzb rax, al\n");
        printf("\tmov %s, rax\n", rd);
        return;
    }
    if (node->kind == ND_LE) {
        printf("\tcmp %s, %s\n", rd, rs);
        printf("\tsetle al\n");
        printf("\tmovzb rax, al\n");
        printf("\tmov %s, rax\n", rd);
        return;
    }
    error_tok(node->tok, "codegen: gen_expr: invalid expression");
}

static void gen_stmt(Node *node) {
    printf(".loc 1 %d\n", node->tok->lineno);
    if (node->kind == ND_BLOCK) {
        for (Node *n = node->body; n; n = n->next) {
            gen_stmt(n);
        }
        return;
    }
    if (node->kind == ND_RETURN) {
        gen_expr(node->lhs);
        printf("\tmov rax, %s\n", reg_pop());
        printf("\tjmp .L.return.%s\n", funcname);
        return;
    }
    if (node->kind == ND_FOR) {
        int lfor = next_label();
        if (node->init)
            gen_stmt(node->init);
        printf(".L.begin.%d:\n", lfor);
        if (node->cond) {
            gen_expr(node->cond);
            printf("\tcmp %s, 0\n", reg_pop());
            printf("\tje .L.end.%d\n", lfor);
        }
        gen_stmt(node->then);
        if (node->inc)
            gen_stmt(node->inc);
        printf("\tjmp .L.begin.%d\n", lfor);
        printf(".L.end.%d:\n", lfor);
        return;
    }
    if (node->kind == ND_IF) {
        gen_expr(node->cond);
        printf("\tcmp %s, 0\n", reg_pop());
        int lif = next_label();
        if (node->els) {
            printf("\tje .L.els.%d\n", lif);
            gen_stmt(node->then);
            printf("\tjmp .L.end.%d\n", lif);
            printf(".L.els.%d:\n", lif);
            gen_stmt(node->els);
        } else {
            printf("\tje .L.end.%d\n", lif);
            gen_stmt(node->then);
        }
        printf(".L.end.%d:\n", lif);
        return;
    }
    if (node->kind == ND_EXPR_STMT) {
        gen_expr(node->lhs);
        reg_pop();
        return;
    }
    error_tok(node->tok, "codegen: gen_stmt: invalid statement");
}

static void emit_data(Program *prog) {
    printf(".data\n");

    for (Var *gv = prog->globals; gv; gv = gv->next) {
        printf("%s:\n", gv->name);
        if (gv->contents)
            for (int i = 0; i < gv->contents_len; i++) {
                printf("\t.byte %d\n", gv->contents[i]);
            }
        else
            printf("\t.zero %d\n", gv->ty->size);
    }
}

static void emit_text(Program *prog) {
    printf(".text\n");
    for (Function *fn = prog->fns; fn; fn = fn->next) {
        printf(".globl %s\n", fn->name);
        printf("%s:\n", fn->name);
        funcname = fn->name;

        // prologue
        // save stack pointer
        printf("\tpush rbp\n");
        printf("\tmov rbp, rsp\n");
        printf("\tsub rsp, %d\n", fn->stacksize);
        // save callee-saved registers
        printf("\tmov [rbp-8], r12\n");
        printf("\tmov [rbp-16], r13\n");
        printf("\tmov [rbp-24], r14\n");
        printf("\tmov [rbp-32], r15\n");

        int i = 0;
        for (Var *v = fn->params; v; v = v->next) {
            i++;
        }
        for (Var *v = fn->params; v; v = v->next) {
            if (v->ty->size == 1) {
                printf("\tmov [rbp-%d], %s\n", v->offset, argreg8[--i]);
            } else {
                printf("\tmov [rbp-%d], %s\n", v->offset, argreg64[--i]);
            }
        }
        for (Node *n = fn->node; n; n = n->next) {
            gen_stmt(n);
            if (top != 0)
                error("top: %d\n", top);
        }

        // Epilogue
        // recover callee-saved registers
        printf(".L.return.%s:\n", funcname);
        printf("\tmov r12, [rbp-8]\n");
        printf("\tmov r13, [rbp-16]\n");
        printf("\tmov r14, [rbp-24]\n");
        printf("\tmov r15, [rbp-32]\n");
        // recover stack pointer
        printf("\tmov rsp, rbp\n");
        printf("\tpop rbp\n");
        printf("\tret\n");
    }
}

void codegen(Program *prog) {
    printf(".intel_syntax noprefix\n");
    emit_data(prog);
    emit_text(prog);
}
