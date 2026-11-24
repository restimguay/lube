/*
 * interp_ops.h - the instructions execute() runs itself (interp.c includes
 * this, once: see "execute(): the loop, in two tiers" there).
 *
 * Each VM_OP(op) { ... } is that instruction's code, written with the VM's
 * registers alone: I, f (the frame), ip (the instruction's own first word),
 * sp, base, consts, and `in` (the instruction's word). The same text is
 * compiled three ways: as a function that ends by calling the next
 * instruction's (tail calls, where the compiler has them), as a label in
 * one function that ends by jumping to the next's (GNU C's computed goto),
 * or as a case of a switch (plain C99).
 *
 * An instruction checks first and changes after: once it has popped,
 * pushed, released or moved ip, it must finish. It ends one of these ways:
 *   NEXT(n)         done: on to the instruction n words on
 *   NEXT_AT()       done: on to the instruction at ip (after a jump, a call)
 *   GO_SLOW()       nothing done: vm_step runs it (and whatever it finds)
 *   GO_RAISE()      what it called raised an error: SYNC() was said first,
 *                   and the Interp's stack holds what it had not let go of
 *   GO_CALLED(st)   op_call or op_invoke returned st
 *   GO_RETURNED(st) op_return returned st
 *   RELEASE_AT(v)   done, but for letting go of v: then on to the instruction at ip
 *   GO_PART(name)   the rest of it is VM_PART(name) { ... }, which reads the instruction again
 *
 * As functions, the common instructions' common cases call nothing (so they
 * need no frame of their own on the C stack: they start at their first
 * useful instruction): what makes a call is a PART (the uncommon cases), or
 * the last thing done (RELEASE_AT, which frees an object only when v was
 * its last owner).
 */

VM_OP(OP_CONST) {
    PUSH(retain(K(ARG_OF(in))));
    NEXT(1);
}

VM_OP(OP_POP) {
    Value v = POP();
    ip += 1;
    RELEASE_AT(v);
}

VM_OP(OP_POPN) {
    for (int n = ARG_OF(in); n > 0; n--) {
        Value v = POP();
        release(v);
    }
    NEXT(1);
}

VM_OP(OP_LOCAL) {
    PUSH(retain(LOCAL(ARG_OF(in))));
    NEXT(1);
}

VM_OP(OP_CAPTURE) {
    PUSH(retain(f->fn->captures[ARG_OF(in)]));
    NEXT(1);
}

VM_OP(OP_SELF) {
    PUSH(retain(OBJ_VAL(f->fn)));
    NEXT(1);
}

/* A global, found where it was (as global_slot finds it), and not a class to make an instance of. */
VM_OP2(OP_GLOBAL, OP_GLOBAL_VALUE) {
    int k = ARG_OF(in), at = CODE->gcache[k];
    ObjMap *gm = I->globals;
    if (LUBE_UNLIKELY(at < 0 || (size_t)at >= gm->used || !gm->entries[at].live || gm->entries[at].key.as.obj != K(k).as.obj))
        GO_SLOW();
    Value v = gm->entries[at].value;
    if (LUBE_UNLIKELY(v.type == VAL_LENT || (OP_OF(in) == OP_GLOBAL_VALUE && IS_CLASS(v)))) GO_SLOW();
    PUSH(retain(v));
    NEXT(1);
}

VM_PART(def_local_other) {  /* (not a Number: its type worked out) */
    int a = ARG_OF(in), at = f->base + a;
    Value *slot = &LOCAL(a), old = *slot;
    I->decl[at] = desc_of(TOP);
    *slot = POP();
    ip += 1;
    RELEASE_AT(old);
}

VM_OP(OP_DEF_LOCAL) {
    int a = ARG_OF(in);
    Value *slot = &LOCAL(a), old = *slot;
    if (LUBE_UNLIKELY(!IS_NUM(TOP))) GO_PART(def_local_other);
    I->decl[f->base + a].kind = TD_NUM;  /* the var keeps this type */
    *slot = POP();
    ip += 1;
    RELEASE_AT(old);
}

VM_OP(OP_CLEAR) {
    Value *s = &LOCAL(ARG_OF(in));
    int n = (int)ip[1];
    for (int i = 0; i < n; i++) {
        Value v = s[i];
        s[i] = NIL_VAL;
        release(v);
    }
    NEXT(2);
}

VM_OP(OP_NEED_VALUE) {
    if (LUBE_UNLIKELY(IS_NIL(TOP))) GO_SLOW();  /* (the error) */
    NEXT(1);
}

VM_OP(OP_LIST) {
    int n = ARG_OF(in);
    ObjList *l = new_list();
    list_reserve(l, (size_t)n);
    Value *from = SP - n;
    for (int i = 0; i < n; i++) l->items[i] = from[i];
    l->count = (size_t)n;
    DROP(n);
    PUSH(OBJ_VAL(l));
    NEXT(1);
}

VM_OP(OP_MAP) {
    int n = ARG_OF(in);
    ObjMap *m = new_map();
    map_reserve(m, (size_t)n);
    Value *from = SP - 2 * n;
    for (int i = 0; i < 2 * n; i += 2) map_set(m, from[i], from[i + 1]);
    DROP(2 * n);
    PUSH(OBJ_VAL(m));
    NEXT(1);
}

VM_OP(OP_CHECK_KEY) {
    Value v = TOP;
    if (LUBE_UNLIKELY(!(IS_BOOL(v) || IS_NUMERIC(v) || IS_STRING(v)))) GO_SLOW();  /* (not as is_key says: the error) */
    NEXT(1);
}

/* a op b: two Numbers; else (binary_other) two strs compared, or the general way. */
VM_PART(binary_other) {
    Value b = PEEK(1), a = PEEK(2), v;
    if (!str_eq_op(ARG_OF(in), a, b, &v) && !str_in_op(ARG_OF(in), a, b, &v)) {
        SYNC();
        v = binary_op(I, (TokenType)ARG_OF(in), a, b);
        if (IS_ERR(v)) {  /* a, then b, let go of before the error, as the general loop does (a File flushes) */
            release(a);
            release(b);
            DROP(2);
            SYNC_REGS();
            GO_RAISE();
        }
    }
    b = PEEK(1);
    a = PEEK(2);
    DROP(1);
    TOP = v;
    release(a);
    release(b);
    NEXT(1);
}

VM_OP(OP_BINARY) {
    Value b = PEEK(1), a = PEEK(2), v;
    if (LUBE_UNLIKELY(!IS_NUM(a) || !IS_NUM(b))) GO_PART(binary_other);
    if (LUBE_UNLIKELY(!num_op(ARG_OF(in), a.as.number, b.as.number, &v))) GO_SLOW();
    DROP(1);
    TOP = v;
    NEXT(1);
}

/* a op b, b (and perhaps a) read in place: a local, or a Number or str written out. */
VM_PART(binary2_other) {
    int arg = ARG_OF(in), bop = arg & 0xff, ka = (arg >> 8) & 3, kb = arg >> 10;
    Value b = kb == OPND_LOCAL ? LOCAL((int)ip[2]) : K(ip[2]);
    Value a = ka == OPND_LOCAL ? LOCAL((int)ip[1]) : ka == OPND_CONST ? K(ip[1]) : TOP, v;
    if (!str_eq_op(bop, a, b, &v) && !str_in_op(bop, a, b, &v)) {  /* the general way: text joined, x in xs... */
        SYNC();
        v = binary_op(I, (TokenType)bop, a, b);
        if (IS_ERR(v)) GO_RAISE();
    }
    if (ka == OPND_STACK) {
        a = TOP;
        TOP = v;
        release(a);
    } else {
        PUSH(v);
    }
    NEXT(3);
}

VM_OP(OP_BINARY2) {
    int arg = ARG_OF(in), bop = arg & 0xff, ka = (arg >> 8) & 3, kb = arg >> 10;
    Value b = kb == OPND_LOCAL ? LOCAL((int)ip[2]) : K(ip[2]);
    Value a = ka == OPND_LOCAL ? LOCAL((int)ip[1]) : ka == OPND_CONST ? K(ip[1]) : TOP, v;
    if (LUBE_UNLIKELY(!IS_NUM(a) || !IS_NUM(b))) GO_PART(binary2_other);
    if (LUBE_UNLIKELY(!num_op(bop, a.as.number, b.as.number, &v))) GO_SLOW();
    if (ka == OPND_STACK) TOP = v;
    else PUSH(v);
    NEXT(3);
}

/* if a < b (or another comparison) is false: jump; a and b as in OP_BINARY2. */
VM_PART(cmp_jump_other) {
    int word = (int)ip[1], bop = word & 0xff, ka = (word >> 8) & 3, kb = word >> 10;
    int stacked = (ka == OPND_STACK) + (kb == OPND_STACK);
    Value b = kb == OPND_LOCAL ? LOCAL((int)ip[3]) : kb == OPND_CONST ? K(ip[3]) : TOP;
    Value a = ka == OPND_LOCAL ? LOCAL((int)ip[2]) : ka == OPND_CONST ? K(ip[2]) : PEEK(stacked), v;
    if (!str_eq_op(bop, a, b, &v) && !str_in_op(bop, a, b, &v)) {
        if (IS_FOREIGN(a) || IS_FOREIGN(b)) GO_SLOW();  /* (a plugin's operator may give what is not a Bool: an error) */
        SYNC();  /* the general way: it is a Bool, or an error */
        v = binary_op(I, (TokenType)bop, a, b);
        if (IS_ERR(v)) {  /* the stacked operands let go of, a first, before the error, as the general loop does */
            if (ka == OPND_STACK) release(a);
            if (kb == OPND_STACK) release(b);
            DROP(stacked);
            SYNC_REGS();
            GO_RAISE();
        }
        if (!IS_BOOL(v)) {  /* (never, of what Lube has itself) */
            release(v);
            GO_SLOW();
        }
    }
    DROP(stacked);
    if (v.as.boolean) ip += 4;
    else GOTO_ARG(in);
    if (ka == OPND_STACK) release(a);
    if (kb == OPND_STACK) release(b);
    NEXT_AT();
}

VM_OP(OP_CMP_JUMP) {
    int word = (int)ip[1], bop = word & 0xff, ka = (word >> 8) & 3, kb = word >> 10;
    int stacked = (ka == OPND_STACK) + (kb == OPND_STACK);
    Value b = kb == OPND_LOCAL ? LOCAL((int)ip[3]) : kb == OPND_CONST ? K(ip[3]) : TOP;
    Value a = ka == OPND_LOCAL ? LOCAL((int)ip[2]) : ka == OPND_CONST ? K(ip[2]) : PEEK(stacked), v;
    if (LUBE_UNLIKELY(!IS_NUM(a) || !IS_NUM(b))) GO_PART(cmp_jump_other);
    if (LUBE_UNLIKELY(!num_op(bop, a.as.number, b.as.number, &v))) GO_SLOW();  /* (else a Bool: two Numbers compared) */
    DROP(stacked);
    if (v.as.boolean) ip += 4;
    else GOTO_ARG(in);
    NEXT_AT();
}

/* x op= value, a statement, x a local var: the Number cases here; the rest, the general way. */
VM_PART(set_local_other) {
    SYNC();
    if (op_assign(I, f, CODE, (int)ip[2], 1) < 0) GO_RAISE();
    LOAD_SP();
    NEXT(3);
}

VM_OP(OP_SET_LOCAL) {
    int word = (int)ip[1], aop = word & 0xff, k = (word >> 8) - 1, a = ARG_OF(in);
    Value rhs = k >= 0 ? K(k) : TOP, *slot = &LOCAL(a);
    if (LUBE_LIKELY(IS_NUM(rhs)) &&
        (aop == T_ASSIGN ? I->decl[f->base + a].kind == TD_NUM  /* (so the value there is a Number too) */
                         : IS_NUM(*slot) && num_op(compound_base(aop), slot->as.number, rhs.as.number, &rhs))) {
        *slot = rhs;
        if (k < 0) DROP(1);
        NEXT(3);
    }
    GO_PART(set_local_other);
}

VM_OP2(OP_ASSIGN, OP_ASSIGN_DROP) {  /* DROP: a statement (the value is not kept) */
    SYNC();
    if (op_assign(I, f, CODE, ARG_OF(in), OP_OF(in) == OP_ASSIGN_DROP) < 0) GO_RAISE();
    LOAD_SP();
    NEXT(1);
}

VM_OP(OP_INTERP) {
    SYNC();
    op_interp(I, ARG_OF(in));
    LOAD_SP();
    NEXT(1);
}

VM_OP(OP_THROW) {
    Value v = POP();
    SYNC();
    lube_throw(I, v);
    GO_RAISE();
}

VM_OP(OP_NEG) {
    Value v = TOP;
    if (LUBE_UNLIKELY(!IS_NUM(v))) GO_SLOW();
    TOP = NUM_VAL(-v.as.number);
    NEXT(1);
}

VM_OP(OP_NOT) {
    Value v = TOP;
    TOP = BOOL_VAL(!is_truthy(v));
    ip += 1;
    RELEASE_AT(v);
}

VM_OP(OP_JUMP) {
    GOTO_ARG(in);
    NEXT_AT();
}

VM_OP(OP_JUMP_IF_FALSE) {
    Value v = POP();
    if (is_truthy(v)) ip += 1;
    else GOTO_ARG(in);
    RELEASE_AT(v);
}

VM_OP2(OP_AND, OP_OR) {
    if (is_truthy(TOP) == (OP_OF(in) == OP_OR)) {  /* keep the deciding value */
        GOTO_ARG(in);
        NEXT_AT();
    } else {
        Value v = POP();
        ip += 1;
        RELEASE_AT(v);
    }
}

VM_OP(OP_LOOP) {
    if (LUBE_UNLIKELY(I->hot)) GO_SLOW();  /* (lube --hot: a loop's turn looks at the files) */
    GOTO_ARG(in);
    NEXT_AT();
}

/* obj[index]: a List and a whole number in range; else (index_other) the general way: an Object's key, a str's
 * character, an index from the end, or the error. */
VM_PART(index_other) {
    SYNC();
    Value v = index_get(I, PEEK(2), PEEK(1));
    if (IS_ERR(v)) {  /* obj, then the index, let go of before the error, as the general loop does */
        Value i2 = PEEK(1), o2 = PEEK(2);
        release(o2);
        release(i2);
        DROP(2);
        SYNC_REGS();
        GO_RAISE();
    }
    Value idx = PEEK(1), obj = PEEK(2);
    DROP(1);
    TOP = v;
    release(obj);
    release(idx);
    NEXT(1);
}

VM_OP(OP_INDEX) {
    Value idx = PEEK(1), obj = PEEK(2);
    double d = IS_NUM(idx) ? idx.as.number : -1;
    if (LUBE_UNLIKELY(!IS_LIST(obj) || !(d >= 0 && d < (double)AS_LIST(obj)->count && d == (double)(size_t)d)))
        GO_PART(index_other);
    DROP(1);
    TOP = retain(AS_LIST(obj)->items[(size_t)d]);
    ip += 1;
    RELEASE_AT(obj);
}

/* obj[index], index (and perhaps obj) read in place: see OP_BINARY2. */
VM_PART(index2_other) {
    int ka = ARG_OF(in) & 3, kb = ARG_OF(in) >> 2;
    Value idx = kb == OPND_LOCAL ? LOCAL((int)ip[2]) : K(ip[2]);
    Value obj = ka == OPND_LOCAL ? LOCAL((int)ip[1]) : ka == OPND_CONST ? K(ip[1]) : TOP;
    SYNC();
    Value v = index_get(I, obj, idx);
    if (IS_ERR(v)) GO_RAISE();
    if (ka == OPND_STACK) {
        obj = TOP;
        TOP = v;
        release(obj);
    } else {
        PUSH(v);
    }
    NEXT(3);
}

VM_OP(OP_INDEX2) {
    int ka = ARG_OF(in) & 3, kb = ARG_OF(in) >> 2;
    Value idx = kb == OPND_LOCAL ? LOCAL((int)ip[2]) : K(ip[2]);
    Value obj = ka == OPND_LOCAL ? LOCAL((int)ip[1]) : ka == OPND_CONST ? K(ip[1]) : TOP;
    double d = IS_NUM(idx) ? idx.as.number : -1;
    if (LUBE_UNLIKELY(!IS_LIST(obj) || !(d >= 0 && d < (double)AS_LIST(obj)->count && d == (double)(size_t)d)))
        GO_PART(index2_other);
    ip += 3;
    if (ka == OPND_STACK) {
        TOP = retain(AS_LIST(obj)->items[(size_t)d]);
        RELEASE_AT(obj);
    }
    PUSH(retain(AS_LIST(obj)->items[(size_t)d]));
    NEXT_AT();
}

/* x.name, x a local holding an instance: read in place; else (field_local_other) the general way: an Object's
 * key, a String's property, a package's member, or the error. */
VM_PART(field_local_other) {
    SYNC();
    Value v = field_get(I, LOCAL(ARG_OF(in)), AS_STRING(K(ip[1])));
    if (IS_ERR(v)) GO_RAISE();
    PUSH(v);
    NEXT(2);
}

VM_OP(OP_FIELD_LOCAL) {
    Value obj = LOCAL(ARG_OF(in));
    int fs = IS_INSTANCE(obj) ? field_slot(AS_MAP(obj), AS_STRING(K(ip[1]))) : -1;
    if (LUBE_UNLIKELY(fs < 0)) GO_PART(field_local_other);
    PUSH(retain(AS_MAP(obj)->entries[fs].value));
    NEXT(2);
}

/* A field of an instance: no hashing; else (field_other) the general way. */
VM_PART(field_other) {
    SYNC();
    Value v = field_get(I, TOP, AS_STRING(K(ARG_OF(in))));
    if (IS_ERR(v)) GO_RAISE();
    Value obj = TOP;
    TOP = v;
    release(obj);
    NEXT(1);
}

VM_OP(OP_FIELD) {
    Value obj = TOP;
    int fs = IS_INSTANCE(obj) ? field_slot(AS_MAP(obj), AS_STRING(K(ARG_OF(in)))) : -1;
    if (LUBE_UNLIKELY(fs < 0)) GO_PART(field_other);
    TOP = retain(AS_MAP(obj)->entries[fs].value);
    ip += 1;
    RELEASE_AT(obj);
}

VM_OP(OP_CLOSURE) {
    SYNC_REGS();  /* (make_closure reads the frame's slots through the Interp) */
    Value c = make_closure(I, CODE->protos[ARG_OF(in)]);
    PUSH(c);
    NEXT(1);
}

VM_OP(OP_CHECK_CALLABLE) {
    Value v = TOP;
    if (LUBE_UNLIKELY(!IS_CALLABLE(v) && !IS_CLASS(v))) GO_SLOW();  /* (the error) */
    NEXT(1);
}

/* f(args). FN: a top-level fn (argc | name constant + 1 << 8). */
VM_PART(call_other) {  /* a class, a function with more to check, a built-in that changes its argument: the long way */
    SYNC();
    GO_CALLED(op_call(I, f, CODE, *ip));
}

VM_PART(call_native) {  /* a built-in that changes none of its arguments, given as many as it takes: called here */
    int argc = OP_OF(in) == OP_CALL_FN ? ARG_OF(in) & 0xff : ARG_OF(in);
    Value *cv = SP - argc - 1;
    ObjNative *nf = AS_NATIVE(*cv);
    if (LUBE_UNLIKELY(nf->changes_first || argc < nf->min_args || (nf->max_args >= 0 && argc > nf->max_args)))
        GO_PART(call_other);  /* (the errors, and what & lends) */
    SYNC();
    Value r = (I->calling = nf)->fn(I, argc, cv + 1);
    if (IS_ERR(r)) GO_RAISE();
    for (Value *p = SP; p > cv;) {  /* its arguments, then the built-in */
        Value gone = *--p;
        release(gone);
    }
    *cv = r;
    SET_SP(cv + 1);
    NEXT(1);
}

VM_OP2(OP_CALL, OP_CALL_FN) {
    int argc = OP_OF(in) == OP_CALL_FN ? ARG_OF(in) & 0xff : ARG_OF(in);
    Value *cv = SP - argc - 1;
    if (LUBE_LIKELY(IS_FUNCTION(*cv))) {
        ObjFunction *fn = AS_FUNCTION(*cv);
        FuncDecl *d = fn->decl;
        int nb = (int)(cv - I->stack) + 1;
#if VM_SLIM_CALL
        /* a plain function, every argument given, nothing to check about them or about what it returns
         * (FuncDecl.fast_argc, worked out at its first call, the long way), and room for its frame: here */
        if (LUBE_LIKELY(d->fast_argc == argc + 1 && !f->last && nb + d->frame_need <= LUBE_STACK_MAX)) {
            Value *top = cv + 1 + d->nslots;
            for (Value *p = SP; p < top; p++) *p = NIL_VAL;
            SET_SP(top);
            f->pc = ip;  /* the caller waits at this call */
            Frame *nf = f + 1;  /* (frames are in a row, up to the last of their chunk) */
            nf->prev = f;
            nf->fn = fn;
            nf->proto = d;
            nf->slots = cv + 1;
            nf->loans = NULL;
            nf->lent = NULL;
            nf->nloans = 0;
            nf->dbg_line = DBG_NO_LINE;
            nf->this_locked = nf->construct = nf->entry = nf->block = nf->slow_ret = 0;
            nf->unused[0] = nf->unused[1] = nf->unused[2] = 0;
            nf->base = nb;
            nf->opbase = nb + d->nslots;
            nf->argc = argc;
            nf->call_base = nb - 1;
            nf->pending = I->npending;
            SET_FRAME(nf);
            ip = d->code.code;
            CALL_REGS(d, cv + 1);
            NEXT_AT();
        }
#else
        /* a plain function: started here (see FuncDecl.plain) */
        int ok = !d->native && d->plain && argc == d->nparams && !f->last && nb + d->nslots + d->code.maxstack <= LUBE_STACK_MAX;
        for (int i = 0; ok && d->types && i < argc; i++) ok = !d->types[i].set || IS_NUM(cv[1 + i]);
        if (ok) {
            Value *top = cv + 1 + d->nslots;
            for (Value *p = SP; p < top; p++) *p = NIL_VAL;
            SET_SP(top);
            f->pc = ip;
            Frame *nf = f + 1;
            frame_init(I, nf, f, fn, d, d->name ? d->name->chars : "<lambda>", d->line, nb, argc, nb - 1, nb + d->nslots);
            SET_FRAME(nf);
            ip = d->code.code;
            CALL_REGS(d, cv + 1);
            NEXT_AT();
        }
#endif
    }
    if (IS_NATIVE(*cv)) GO_PART(call_native);
    GO_PART(call_other);
}

/* x.name(: x a local holding an instance of the class last seen here, whose method that is (as op_method_prep
 * finds, and prep_method): the method and x's slot pushed, and the call pending, lending x when x can change. */
VM_PART(method_prep_other) {
    SYNC();
    if (op_method_prep(I, CODE, &CODE->methods[ARG_OF(in)]) < 0) GO_RAISE();
    LOAD_SP();
    NEXT(1);
}

VM_OP(OP_METHOD_PREP) {
#if VM_SLIM_CALL
    MethodDesc *md = &CODE->methods[ARG_OF(in)];
    if (LUBE_LIKELY(md->recv_place >= 0)) {
        const PlaceDesc *pd = &CODE->places[md->recv_place];
        const VarRef *rv = &pd->var;
        Value *root = &LOCAL(rv->index);  /* (looked at only when x is a local) */
        if (LUBE_LIKELY(pd->nkeys == 0 && rv->kind == VAR_LOCAL && IS_INSTANCE(*root) && AS_MAP(*root)->klass == md->cache_class &&
                        md->cache_epoch == I->epoch && !IS_NIL(md->cache_method) && I->npending < I->cap_pending)) {
            PendingCall *p = &I->pending[I->npending++];
            p->callee = (int)(SP - I->stack);
            p->has_slot = 1;
            p->place.nkeys = 0;
            PUSH(retain(md->cache_method));
            if (rv->changeable && !(rv->origin == ORIGIN_THIS && f->this_locked)) {  /* (as place_changeable says) */
                p->mode = SELF_LEND;
                p->place.root = rv;
                PUSH(NIL_VAL);
            } else {  /* x cannot change: the method gets its value, and may not change it either */
                p->mode = SELF_LOCKED;
                PUSH(retain(*root));
            }
            NEXT(1);
        }
        /* xs.push(, s.ends_with(: x a local that is no Object or instance, and `name` a built-in (as op_method_prep
         * finds it): the built-in, with x lent when it changes its first argument, else x's value */
        if (pd->nkeys == 0 && rv->kind == VAR_LOCAL &&
            (root->type == VAL_STR || (root->type == VAL_OBJ && !IS_MAP(*root) && !IS_PACKAGE(*root) && !LUBE_OWN_METHODS(OBJ_TYPE(*root)))) &&
            md->fallback.kind == VAR_GLOBAL && I->npending < I->cap_pending) {
            Value *fn = global_cached(I, &md->fallback);
            if (fn && IS_NATIVE(*fn)) {
                PendingCall *p = &I->pending[I->npending++];
                p->callee = (int)(SP - I->stack);
                p->has_slot = 1;
                p->place.nkeys = 0;
                PUSH(retain(*fn));
                if (AS_NATIVE(*fn)->changes_first) {
                    p->mode = SELF_LEND;
                    p->place.root = rv;
                    PUSH(NIL_VAL);
                } else {
                    p->mode = SELF_VALUE;
                    PUSH(retain(*root));
                }
                NEXT(1);
            }
        }
        /* math.sqrt(: a package in a global, and its function last called here still where it was (as
         * op_method_prep finds it, and package_callee) */
        ObjMap *g = I->globals;
        int at = md->cache_gslot - 1;
        if (pd->nkeys == 0 && rv->kind == VAR_GLOBAL && (root = global_cached(I, rv)) != NULL && IS_PACKAGE(*root) &&
            md->cache_pkg == AS_PACKAGE(*root)->name && at >= 0 && (size_t)at < g->used && g->entries[at].live &&
            g->entries[at].key.as.obj == (Obj *)md->cache_key && IS_CALLABLE(g->entries[at].value) &&
            I->npending < I->cap_pending) {
            PendingCall *p = &I->pending[I->npending++];
            p->callee = (int)(SP - I->stack);
            p->has_slot = 1;
            p->place.nkeys = 0;
            p->mode = SELF_NONE;
            PUSH(retain(g->entries[at].value));
            PUSH(NIL_VAL);
            NEXT(1);
        }
    }
#endif
    GO_PART(method_prep_other);
}

/* ...args): the pending call starts. A plain method (FuncDecl.fast_margc) called on a local, with room for its
 * frame: here, x moved into the method's `this` when it is lent (and the frame says whose it is: Frame.lent). */
VM_PART(invoke_other) {
    SYNC();
    GO_CALLED(op_invoke(I, f, CODE, &CODE->calls[ARG_OF(in)]));
}

/* x.f(args), f a built-in: one that takes x by value (s.ends_with(t)), a package's (math.sqrt(x), with no x in
 * front), or one that changes x, a local that can change and that nothing watches (xs.push(v): x is lent to it,
 * and given back), given as many arguments as it takes: called here. */
VM_PART(invoke_native) {
    const CallDesc *cd = &CODE->calls[ARG_OF(in)];
    PendingCall *pc = &I->pending[I->npending - 1];
    Value *cv = I->stack + pc->callee;
    ObjNative *nf = AS_NATIVE(*cv);
    const VarRef *rv = pc->place.root;
    int lend = pc->mode == SELF_LEND, self = lend || pc->mode == SELF_VALUE, argc = self + cd->nargs;
    if (LUBE_UNLIKELY(cd->refs || !pc->has_slot || !(self || pc->mode == SELF_NONE) || argc < nf->min_args ||
                      (nf->max_args >= 0 && argc > nf->max_args) ||
                      (lend ? !nf->changes_first || pc->place.nkeys != 0 || rv->kind != VAR_LOCAL || !rv->changeable ||
                                  (rv->origin == ORIGIN_THIS && f->this_locked) || (rv->watch && rv->watch->n)
                            : nf->changes_first)))
        GO_PART(invoke_other);  /* (the errors, and the other ways to lend) */
    I->npending--;
    if (lend) {  /* x is lent to it (in use meanwhile) */
        Value *root = &LOCAL(rv->index);
        cv[1] = *root;
        *root = NIL_VAL;
    }
    SYNC();
    Value r = (I->calling = nf)->fn(I, argc, cv + 2 - self);
    if (lend) {  /* ...and given back, whatever happened (pc may have moved: the built-in may have made calls) */
        LOCAL(rv->index) = cv[1];
        cv[1] = NIL_VAL;
    }
    if (IS_ERR(r)) GO_RAISE();
    for (Value *p = SP; p > cv;) {  /* its arguments, x's slot, then the built-in */
        Value gone = *--p;
        release(gone);
    }
    *cv = r;
    SET_SP(cv + 1);
    NEXT(1);
}

VM_OP(OP_INVOKE) {
#if VM_SLIM_CALL
    const CallDesc *cd = &CODE->calls[ARG_OF(in)];
    PendingCall *pc = &I->pending[I->npending - 1];
    Value *cv = I->stack + pc->callee;
    if (LUBE_LIKELY(!cd->refs && pc->has_slot && IS_FUNCTION(*cv))) {
        ObjFunction *fn = AS_FUNCTION(*cv);
        FuncDecl *d = fn->decl;
        int argc = 1 + cd->nargs, nb = pc->callee + 1;
        const VarRef *rv = pc->place.root;
        int lend = pc->mode == SELF_LEND;
        if (LUBE_LIKELY(d->fast_margc == argc + 1 && !f->last && nb + d->frame_need <= LUBE_STACK_MAX &&
                        (lend ? pc->place.nkeys == 0 && rv->kind == VAR_LOCAL && !(rv->watch && rv->watch->n)
                              : pc->mode == SELF_LOCKED))) {
            I->npending--;
            if (lend) {
                Value *root = &LOCAL(rv->index);
                cv[1] = *root;  /* (its arguments are in place after it) */
                *root = NIL_VAL;
            }
            Value *top = cv + 1 + d->nslots;
            for (Value *p = SP; p < top; p++) *p = NIL_VAL;
            SET_SP(top);
            f->pc = ip;  /* the caller waits at this call */
            Frame *nf = f + 1;
            nf->prev = f;
            nf->fn = fn;
            nf->proto = d;
            nf->slots = cv + 1;
            nf->loans = NULL;
            nf->lent = lend ? rv : NULL;
            nf->nloans = 0;
            nf->dbg_line = DBG_NO_LINE;
            nf->this_locked = (unsigned char)!lend;
            nf->construct = nf->entry = nf->block = 0;
            nf->slow_ret = lend ? 2 : 0;
            nf->unused[0] = nf->unused[1] = nf->unused[2] = 0;
            nf->base = nb;
            nf->opbase = nb + d->nslots;
            nf->argc = argc;
            nf->call_base = nb - 1;
            nf->pending = I->npending;
            SET_FRAME(nf);
            ip = d->code.code;
            CALL_REGS(d, cv + 1);
            NEXT_AT();
        }
    }
    if (IS_NATIVE(*cv)) GO_PART(invoke_native);
#endif
    GO_PART(invoke_other);
}

#if VM_SLIM_CALL
/* A plain call's frame returns: its result is in the callee's slot already; the frame's slots and the arguments
 * go, then the callee (the frame's function: what was in that slot), and the caller goes on after its call. */
VM_PART(return_free) {  /* ...where one of them has no other owner: the same, freeing as it goes */
    ObjFunction *callee = f->fn;
    Value *bot = f->slots, *p = SP;
    while (p > bot) {
        Value gone = *--p;
        release(gone);
    }
    SET_SP(bot);
    SET_FRAME(f->prev);
    ip = f->pc + 1;
    CALL_REGS(f->proto, f->slots);
    if (--callee->obj.rc == 0) obj_destroy(&callee->obj);
    NEXT_AT();
}

#ifdef VM_PC_CHECK  /* (a checking build: what OP_RETURN no longer looks at) */
#define CHECK_PLAIN_RETURN()                                                                                         \
    do {                                                                                                             \
        if (I->npending != f->pending || f->entry || f->construct || f->block || f->loans || f->slots[-1].type != VAL_OBJ || \
            f->call_base != f->base - 1 || (I->nfwatch && I->fwatch[I->nfwatch - 1].depth >= f->index + 1))          \
            abort();                                                                                                 \
    } while (0)
#else
#define CHECK_PLAIN_RETURN() ((void)0)
#endif
#define PLAIN_RETURN()                                                             \
    do {                                                                           \
        ObjFunction *callee = f->fn;                                               \
        Value *bot = f->slots, *p = SP;                                            \
        for (; p > bot; p--) {                                                     \
            Value gone = p[-1];                                                    \
            if (gone.type != VAL_OBJ) continue;                                    \
            if (LUBE_UNLIKELY(gone.as.obj->rc == 1)) break; /* (its last owner) */ \
            gone.as.obj->rc--;                                                     \
        }                                                                          \
        SET_SP(p);                                                                 \
        if (LUBE_UNLIKELY(p > bot || callee->obj.rc == 1)) GO_PART(return_free);   \
        callee->obj.rc--;                                                          \
        SET_FRAME(f->prev);                                                        \
        ip = f->pc + 1; /* past the call it waited on */                           \
        CALL_REGS(f->proto, f->slots);                                             \
        NEXT_AT();                                                                 \
    } while (0)

/* A method's frame that holds the caller's x as its `this` returns (its result is in the callee's slot): as a
 * plain call's, and x gets its value back. */
VM_PART(return_lent) {
    ObjFunction *callee = f->fn;
    const VarRef *rv = f->lent;
    Value *bot = f->slots, *p = SP, self = bot[0];
    while (p > bot + 1) {
        Value gone = *--p;
        release(gone);
    }
    SET_SP(bot);
    SET_FRAME(f->prev);
    ip = f->pc + 1;
    CALL_REGS(f->proto, f->slots);
    if (--callee->obj.rc == 0) obj_destroy(&callee->obj);
    Value *root = &LOCAL(rv->index), old = *root;
    *root = self;
    release(old);
    NEXT_AT();
}

VM_PART(return_other) {  /* (a frame with more to do when it ends: see Frame.slow_ret) */
    Value v = POP();
    if (f->slow_ret == 2) {
        f->slots[-1] = v;
        GO_PART(return_lent);
    }
    SYNC();
    GO_RETURNED(op_return(I, f, v));
}

VM_OP(OP_RETURN) {
    if (LUBE_UNLIKELY(f->slow_ret)) GO_PART(return_other);
    CHECK_PLAIN_RETURN();
    Value v = POP();
    f->slots[-1] = v;
    PLAIN_RETURN();
}

VM_PART(return_nil_other) {
    if (f->slow_ret == 2) {
        f->slots[-1] = NIL_VAL;
        GO_PART(return_lent);
    }
    SYNC();
    GO_RETURNED(op_return(I, f, NIL_VAL));
}

VM_OP(OP_RETURN_NIL) {
    if (LUBE_UNLIKELY(f->slow_ret)) GO_PART(return_nil_other);
    CHECK_PLAIN_RETURN();
    f->slots[-1] = NIL_VAL;
    PLAIN_RETURN();
}
#else
/* Nothing else to do (see leave): its slots, the arguments and the callee go. */
VM_OP2(OP_RETURN, OP_RETURN_NIL) {
    Value v = OP_OF(in) == OP_RETURN ? POP() : NIL_VAL;
    if ((f->fn && (f->proto->ret.set || f->proto->strict)) || f->entry || f->construct || f->loans || f->block ||
        I->npending != f->pending || (I->nfwatch && I->fwatch[I->nfwatch - 1].depth >= f->index + 1)) {
        SYNC();
        GO_RETURNED(op_return(I, f, v));
    }
    Value *bot = I->stack + f->call_base, *p = SP;
    while (p > bot) {
        Value gone = *--p;
        *p = NIL_VAL;
        release(gone);
    }
    SET_FRAME(f->prev);
    ip = f->pc + 1;
    CALL_REGS(f->proto, f->slots);
    *bot = v;
    SET_SP(bot + 1);
    NEXT_AT();
}
#endif

/* The loop owns what it iterates: changes to the variable go to a copy. */
VM_OP(OP_FOR_PREP) {
    Value it = TOP;
    if (IS_RANGE(it)) PUSH(NUM_VAL(range_length(AS_RANGE(it))));  /* (a range: its length) */
    else if (LUBE_LIKELY(IS_LIST(it) || IS_MAP(it) || IS_TEXT(it) || IS_BYTES(it))) PUSH(NUM_VAL(0));  /* position */
    else GO_SLOW();  /* (a channel, a plugin's object, or the error) */
    PUSH(NUM_VAL(0));  /* count */
    NEXT(1);
}

/* for x in xs, over a Range or a List; the other ways to loop (for i, x in xs; over an Object), in for_next_other;
 * the rest, the long way. */
VM_PART(for_next_other) {
    Value *st = SP - 3;  /* the loop's own: what it goes over, its position, its count */
    int s1 = (int)ip[1], s2 = (int)ip[2];
    double k = st[2].as.number;
    Value item;
    if (IS_RANGE(st[0])) {
        if (k >= st[1].as.number) {  /* (its length, from OP_FOR_PREP) */
            GOTO_ARG(in);
            NEXT_AT();
        }
        ObjRange *rg = AS_RANGE(st[0]);
        item = NUM_VAL(rg->start + k * rg->step);
    } else if (IS_LIST(st[0])) {
        ObjList *l = AS_LIST(st[0]);
        if (k >= (double)l->count) {
            GOTO_ARG(in);
            NEXT_AT();
        }
        item = retain(l->items[(size_t)k]);
    } else if (IS_MAP(st[0])) {  /* keys (and values) */
        ObjMap *m = AS_MAP(st[0]);
        size_t i = (size_t)st[1].as.number;
        while (i < m->used && !m->entries[i].live) i++;
        if (i >= m->used) {
            GOTO_ARG(in);
            NEXT_AT();
        }
        Value key = retain(m->entries[i].key), *ks = &LOCAL(s1), kold = *ks;
        st[1] = NUM_VAL((double)(i + 1));
        st[2] = NUM_VAL(k + 1);
        *ks = key;
        if (s2 >= 0) {
            Value *vs = &LOCAL(s2), vold = *vs;
            *vs = retain(m->entries[i].value);
            release(kold);
            release(vold);
        } else {
            release(kold);
        }
        NEXT(3);
    } else {
        GO_SLOW();
    }
    st[2] = NUM_VAL(k + 1);
    if (s2 < 0) {  /* for x in xs */
        Value *slot = &LOCAL(s1), old = *slot;
        *slot = item;
        release(old);
    } else {  /* for i, x in xs */
        Value *is = &LOCAL(s1), *xs = &LOCAL(s2), iold = *is, xold = *xs;
        *is = NUM_VAL(k);
        *xs = item;
        release(iold);
        release(xold);
    }
    NEXT(3);
}

VM_OP(OP_FOR_NEXT) {
    Value *st = SP - 3;
    double k = st[2].as.number;
    Value item;
    if (LUBE_UNLIKELY((int)ip[2] >= 0)) GO_PART(for_next_other);
    if (IS_RANGE(st[0])) {
        if (k >= st[1].as.number) {  /* (its length, from OP_FOR_PREP) */
            GOTO_ARG(in);
            NEXT_AT();
        }
        ObjRange *rg = AS_RANGE(st[0]);
        item = NUM_VAL(rg->start + k * rg->step);
    } else if (IS_LIST(st[0])) {
        ObjList *l = AS_LIST(st[0]);
        if (k >= (double)l->count) {
            GOTO_ARG(in);
            NEXT_AT();
        }
        item = retain(l->items[(size_t)k]);
    } else {
        GO_PART(for_next_other);
    }
    st[2] = NUM_VAL(k + 1);
    Value *slot = &LOCAL((int)ip[1]), old = *slot;
    *slot = item;
    ip += 3;
    RELEASE_AT(old);
}

VM_OP(OP_WATCH_BEFORE) {  /* (nothing to compare with when no `on` block watches anything) */
    if (LUBE_UNLIKELY(I->nwatchers || I->nfwatch)) GO_SLOW();
    PUSH(NIL_VAL);
    NEXT(1);
}

VM_OP(OP_WATCH_AFTER) {
    if (LUBE_UNLIKELY(!IS_NIL(PEEK(2)))) GO_SLOW();
    PEEK(2) = PEEK(1);
    DROP(1);
    NEXT(1);
}

VM_OP(OP_ARG_DEFAULT) {  /* jumps over a default's code when the argument was given */
    if (f->argc > ARG_OF(in)) {
        ip = CODE->code + (int)ip[1];
        NEXT_AT();
    }
    NEXT(2);
}

/* xs.push(v), push(&xs, v): may OP_PUSH_DO append v? Else on to the call, at ip[1]. */
VM_OP(OP_PUSH_CHECK) {
    MethodDesc *md = &CODE->methods[ARG_OF(in)];
    const VarRef *rv = &CODE->places[md->recv_place].var;
    Value *root = &LOCAL(rv->index);
    if (((IS_LIST(*root) && AS_LIST(*root)->elem == TT_ANY) || IS_BYTES(*root)) && !(rv->watch && rv->watch->n)) {
        Value *push = global_cached(I, &md->fallback);
        if (LUBE_UNLIKELY(!push)) GO_SLOW();  /* (found the long way, and where it is remembered) */
        if (IS_NATIVE(*push) && AS_NATIVE(*push)->changes_first == 2) NEXT(2);
    }
    GOTO_WORD(ip[1]);
    NEXT_AT();
}

/* ...appended as n_push does it (to a copy, if the List is shared: xs.push(xs)). */
VM_OP(OP_PUSH_DO) {
    Value *root = &LOCAL(ARG_OF(in));
    if (LUBE_UNLIKELY(!IS_LIST(*root))) GO_SLOW();  /* (Bytes) */
    if (LUBE_UNLIKELY(root->as.obj->rc != 1)) make_unique(root);
    ObjList *l = AS_LIST(*root);
    if (LUBE_LIKELY(l->count < l->capacity)) l->items[l->count++] = TOP;  /* (the List takes it over) */
    else list_push(l, TOP);
    GOTO_WORD(ip[1]);
    if (OP_OF(*ip) == OP_POP) {  /* (a statement: its nothing would be dropped) */
        ip++;
        DROP(1);
    } else {
        TOP = NIL_VAL;
    }
    NEXT_AT();
}
