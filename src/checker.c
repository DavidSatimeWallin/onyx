#define BH_DEBUG
#include "parser.h"
#include "utils.h"

// All of the `check` functions return a boolean that signals if an issue
// was reached while processing the node. These error booleans propagate
// up the call stack until they reach `check_entity`.

#define CHECK(kind, ...) do { \
    CheckStatus cs = check_ ## kind (__VA_ARGS__); \
    if (cs > Check_Errors_Start) return cs; \
    } while (0)

#define YIELD(loc, msg) do { \
    if (context.cycle_detected) { \
        onyx_report_error(loc, Error_Waiting_On, msg); \
        return Check_Error; \
    } else { \
        return Check_Yield_Macro; \
    } \
    } while (0)

#define YIELD_(loc, msg, ...) do { \
    if (context.cycle_detected) { \
        onyx_report_error(loc, Error_Waiting_On, msg, __VA_ARGS__); \
        return Check_Error; \
    } else { \
        return Check_Yield_Macro; \
    } \
    } while (0)

#define YIELD_ERROR(loc, msg) do { \
    if (context.cycle_detected) { \
        onyx_report_error(loc, Error_Critical, msg); \
        return Check_Error; \
    } else { \
        return Check_Yield_Macro; \
    } \
    } while (0)

#define ERROR(loc, msg) do { \
    onyx_report_error(loc, Error_Critical, msg); \
    return Check_Error; \
    } while (0)

#define ERROR_(loc, msg, ...) do { \
    onyx_report_error(loc, Error_Critical, msg, __VA_ARGS__); \
    return Check_Error; \
    } while (0)

#define TYPE_CHECK_(expr, type, type_name)                                                      \
    TypeMatch type_name;                                                                        \
    type_name = unify_node_and_type(expr, type);                                                \
    if (type_name == TYPE_MATCH_YIELD) YIELD((*expr)->token->pos, "Waiting on type checking."); \
    if (type_name == TYPE_MATCH_FAILED)

#define CONCAT(a, b) a##_##b
#define DEFER_LINE(a, line) CONCAT(a, line)
#define TYPE_CHECK(expr, type) TYPE_CHECK_(expr, type, DEFER_LINE(tc, __LINE__))

typedef enum CheckStatus {
    Check_Success,  // The node was successfully checked with out errors
    Check_Complete, // The node is done processing

    Check_Errors_Start,
    Check_Return_To_Symres, // Return this node for further symres processing
    Check_Yield_Macro,
    Check_Failed,           // The node is done processing and should be put in the state of Failed.
    Check_Error,    // There was an error when checking the node
} CheckStatus;

CheckStatus check_block(AstBlock* block);
CheckStatus check_statement_chain(AstNode** start);
CheckStatus check_statement(AstNode** pstmt);
CheckStatus check_return(AstReturn* retnode);
CheckStatus check_if(AstIfWhile* ifnode);
CheckStatus check_while(AstIfWhile* whilenode);
CheckStatus check_for(AstFor* fornode);
CheckStatus check_switch(AstSwitch* switchnode);
CheckStatus check_call(AstCall** pcall);
CheckStatus check_binaryop(AstBinaryOp** pbinop);
CheckStatus check_unaryop(AstUnaryOp** punop);
CheckStatus check_struct_literal(AstStructLiteral* sl);
CheckStatus check_array_literal(AstArrayLiteral* al);
CheckStatus check_range_literal(AstRangeLiteral** range);
CheckStatus check_compound(AstCompound* compound);
CheckStatus check_if_expression(AstIfExpression* if_expr);
CheckStatus check_expression(AstTyped** expr);
CheckStatus check_address_of(AstAddressOf** paof);
CheckStatus check_dereference(AstDereference* deref);
CheckStatus check_subscript(AstSubscript** paa);
CheckStatus check_field_access(AstFieldAccess** pfield);
CheckStatus check_method_call(AstBinaryOp** mcall);
CheckStatus check_size_of(AstSizeOf* so);
CheckStatus check_align_of(AstAlignOf* ao);
CheckStatus check_global(AstGlobal* global);
CheckStatus check_function(AstFunction* func);
CheckStatus check_overloaded_function(AstOverloadedFunction* func);
CheckStatus check_struct(AstStructType* s_node);
CheckStatus check_temp_function_header(AstFunction* func);
CheckStatus check_function_header(AstFunction* func);
CheckStatus check_memres_type(AstMemRes* memres);
CheckStatus check_memres(AstMemRes* memres);
CheckStatus check_type(AstType** ptype);
CheckStatus check_insert_directive(AstDirectiveInsert** pinsert);
CheckStatus check_directive_solidify(AstDirectiveSolidify** psolid);
CheckStatus check_do_block(AstDoBlock** pdoblock);
CheckStatus check_constraint(AstConstraint *constraint);
CheckStatus check_constraint_context(ConstraintContext *cc, Scope *scope, OnyxFilePos pos);
CheckStatus check_polyquery(AstPolyQuery *query);

// HACK HACK HACK
b32 expression_types_must_be_known = 0;
b32 all_checks_are_final           = 1;
b32 inside_for_iterator            = 0;

#define STATEMENT_LEVEL 1
#define EXPRESSION_LEVEL 2
u32 current_checking_level=0;

static inline void fill_in_type(AstTyped* node) {
    if (node->type == NULL) {
        if (check_type(&node->type_node) > Check_Errors_Start) return;

        node->type = type_build_from_ast(context.ast_alloc, node->type_node);
    }
}

// HACK: This should be baked into a structure, not a global variable.
static Type** expected_return_type = NULL;

CheckStatus check_return(AstReturn* retnode) {
    if (retnode->expr) {
        CHECK(expression, &retnode->expr);

        if (*expected_return_type == &type_auto_return) {
            resolve_expression_type(retnode->expr);
            if (retnode->expr->type == NULL)
                YIELD_ERROR(retnode->token->pos, "Unable to determine the automatic return type here.");

            *expected_return_type = retnode->expr->type;
            return Check_Success;
        }

        TYPE_CHECK(&retnode->expr, *expected_return_type) {
            ERROR_(retnode->token->pos,
                    "Expected to return a value of type '%s', returning value of type '%s'.",
                    type_get_name(*expected_return_type),
                    node_get_type_name(retnode->expr));
        }

        //
        // Catch the obvious case of return '^.{ ... }', as that will never
        // be legal.
        if (retnode->expr->kind == Ast_Kind_Address_Of) {
            AstAddressOf *aof = (AstAddressOf *) retnode->expr;
            if (node_is_addressable_literal((AstNode *) aof->expr)) {
                ERROR(retnode->token->pos, "Cannot return a pointer to a literal, as the space reserved for the literal will be freed upon returning.");
            }
        }

    } else {
        if (*expected_return_type == &type_auto_return) {
            *expected_return_type = &basic_types[Basic_Kind_Void];
            return Check_Success;
        }

        if ((*expected_return_type)->Basic.size > 0) {
            ERROR_(retnode->token->pos,
                "Returning from non-void function without a value. Expected a value of type '%s'.",
                type_get_name(*expected_return_type));
        }
    }

    return Check_Success;
}

CheckStatus check_if(AstIfWhile* ifnode) {
    if (ifnode->initialization != NULL) CHECK(statement_chain, &ifnode->initialization);

    if (ifnode->kind == Ast_Kind_Static_If) {
        if ((ifnode->flags & Ast_Flag_Static_If_Resolved) == 0) {
            YIELD(ifnode->token->pos, "Waiting for static if to be resolved.");
        }

        if (static_if_resolution(ifnode)) {
            if (ifnode->true_stmt != NULL) {
                CHECK(statement, (AstNode **) &ifnode->true_stmt);
                ifnode->true_stmt->rules = Block_Rule_Macro;
            }

        } else {
            if (ifnode->false_stmt != NULL) {
                CHECK(statement, (AstNode **) &ifnode->false_stmt);
                ifnode->false_stmt->rules = Block_Rule_Macro;
            }
        }

    } else {
        CHECK(expression, &ifnode->cond);

        if (!type_is_bool(ifnode->cond->type)) {
            ERROR_(ifnode->cond->token->pos, "Expected expression of type 'bool' for condition, got '%s'", type_get_name(ifnode->cond->type));
        }

        if (ifnode->true_stmt)  CHECK(statement, (AstNode **) &ifnode->true_stmt);
        if (ifnode->false_stmt) CHECK(statement, (AstNode **) &ifnode->false_stmt);
    }

    return Check_Success;
}

CheckStatus check_while(AstIfWhile* whilenode) {
    if (whilenode->initialization != NULL) CHECK(statement_chain, &whilenode->initialization);

    CHECK(expression, &whilenode->cond);

    if (!type_is_bool(whilenode->cond->type)) {
        ERROR_(whilenode->cond->token->pos, "Expected expression of type 'bool' for condition, got '%s'", type_get_name(whilenode->cond->type));
    }

    if (whilenode->true_stmt)  CHECK(statement, (AstNode **) &whilenode->true_stmt);
    if (whilenode->false_stmt) {
        if (whilenode->bottom_test) {
            ERROR(whilenode->token->pos, "while-loops with an 'else' clause cannot be bottom tested.");
        }

        CHECK(statement, (AstNode **) &whilenode->false_stmt);
    }

    return Check_Success;
}

CheckStatus check_for(AstFor* fornode) {
    b32 old_inside_for_iterator;
    if (fornode->flags & Ast_Flag_Has_Been_Checked) goto fornode_expr_checked;

    CHECK(expression, &fornode->iter);
    resolve_expression_type(fornode->iter);

    Type* iter_type = fornode->iter->type;
    if (iter_type == NULL) YIELD(fornode->token->pos, "Waiting for iteration expression type to be known.");

    OnyxFilePos error_loc = fornode->var->token->pos;
    if (error_loc.filename == NULL) {
        error_loc = fornode->token->pos;
    }

    fornode->loop_type = For_Loop_Invalid;
    if (types_are_compatible(iter_type, &basic_types[Basic_Kind_I32])) {
        if (fornode->by_pointer) {
            ERROR(error_loc, "Cannot iterate by pointer over a range.");
        }

        AstNumLit* low_0    = make_int_literal(context.ast_alloc, 0);
        AstRangeLiteral* rl = make_range_literal(context.ast_alloc, (AstTyped *) low_0, fornode->iter);
        CHECK(range_literal, &rl);
        fornode->iter = (AstTyped *) rl;

        fornode->var->type = builtin_range_type_type->Struct.memarr[0]->type;
        fornode->var->flags |= Ast_Flag_Cannot_Take_Addr;
        fornode->loop_type = For_Loop_Range;
    }
    else if (types_are_compatible(iter_type, builtin_range_type_type)) {
        if (fornode->by_pointer) {
            ERROR(error_loc, "Cannot iterate by pointer over a range.");
        }

        // NOTE: Blindly copy the first range member's type which will
        // be the low value.                - brendanfh 2020/09/04
        fornode->var->type = builtin_range_type_type->Struct.memarr[0]->type;
        fornode->var->flags |= Ast_Flag_Cannot_Take_Addr;
        fornode->loop_type = For_Loop_Range;

    }
    else if (iter_type->kind == Type_Kind_Array) {
        if (fornode->by_pointer) fornode->var->type = type_make_pointer(context.ast_alloc, iter_type->Array.elem);
        else                     fornode->var->type = iter_type->Array.elem;

        fornode->loop_type = For_Loop_Array;
    }
    else if (iter_type->kind == Type_Kind_Slice) {
        if (fornode->by_pointer) fornode->var->type = type_make_pointer(context.ast_alloc, iter_type->Slice.elem);
        else                     fornode->var->type = iter_type->Slice.elem;

        fornode->loop_type = For_Loop_Slice;

    }
    else if (iter_type->kind == Type_Kind_VarArgs) {
        if (fornode->by_pointer) {
            ERROR_(error_loc, "Cannot iterate by pointer over '%s'.", type_get_name(iter_type));
        }

        fornode->var->type = iter_type->VarArgs.elem;

        // NOTE: Slices are VarArgs are being treated the same here.
        fornode->loop_type = For_Loop_Slice;
    }
    else if (iter_type->kind == Type_Kind_DynArray) {
        if (fornode->by_pointer) fornode->var->type = type_make_pointer(context.ast_alloc, iter_type->DynArray.elem);
        else                     fornode->var->type = iter_type->DynArray.elem;

        fornode->loop_type = For_Loop_DynArr;
    }
    else if (type_struct_constructed_from_poly_struct(iter_type, builtin_iterator_type)) {
        if (fornode->by_pointer) {
            ERROR(error_loc, "Cannot iterate by pointer over an iterator.");
        }

        // HACK: This assumes the Iterator type only has a single type argument.
        fornode->var->type = iter_type->Struct.poly_sln[0].type;
        fornode->loop_type = For_Loop_Iterator;
    }

    if (fornode->by_pointer)
        fornode->var->flags |= Ast_Flag_Cannot_Take_Addr;

    if (fornode->loop_type == For_Loop_Invalid)
        ERROR_(error_loc, "Cannot iterate over a '%s'.", type_get_name(iter_type));

    if (fornode->no_close && fornode->loop_type != For_Loop_Iterator) {
        onyx_report_warning(error_loc, "Warning: #no_close here is meaningless as the iterable is not an iterator.");
    }

    fornode->flags |= Ast_Flag_Has_Been_Checked;

fornode_expr_checked:
    old_inside_for_iterator = inside_for_iterator;
    inside_for_iterator = 0;
    iter_type = fornode->iter->type;
    if (type_struct_constructed_from_poly_struct(iter_type, builtin_iterator_type)) {
        inside_for_iterator = 1;
    }

    do {
        CheckStatus cs = check_block(fornode->stmt);
        inside_for_iterator = old_inside_for_iterator;
        if (cs > Check_Errors_Start) return cs; 
    } while(0);

    return Check_Success;
}

static b32 add_case_to_switch_statement(AstSwitch* switchnode, u64 case_value, AstBlock* block, OnyxFilePos pos) {
    assert(switchnode->switch_kind == Switch_Kind_Integer);

    switchnode->min_case = bh_min(switchnode->min_case, case_value);
    switchnode->max_case = bh_max(switchnode->max_case, case_value);

    if (bh_imap_has(&switchnode->case_map, case_value)) {
        onyx_report_error(pos, Error_Critical, "Multiple cases for values '%d'.", case_value);
        return 1;
    }

    bh_imap_put(&switchnode->case_map, case_value, (u64) block);
    return 0;
}

static CheckStatus collect_switch_case_blocks(AstSwitch* switchnode, AstBlock* root) {
    AstNode *walker = root->body;
    while (walker != NULL) {
        switch (walker->kind) {
            case Ast_Kind_Block:
                collect_switch_case_blocks(switchnode, (AstBlock *) walker);
                break;

            case Ast_Kind_Switch_Case: {
                AstSwitchCase *case_node = (AstSwitchCase *) walker;
                if (case_node->is_default) {
                    if (switchnode->default_case != NULL && switchnode->default_case != case_node->block) {
                        ERROR(case_node->token->pos, "Multiple #default cases given");
                        ERROR(switchnode->default_case->token->pos, "Multiple #default cases given");
                        return Check_Error;
                    }

                    switchnode->default_case = case_node->block;
                } else {
                    bh_arr_push(switchnode->cases, case_node);
                }
                break;
            }

            default:
                ERROR(walker->token->pos, "This statement is not allowed here.");
        }

        walker = walker->next;
    }

    return Check_Success;
}

CheckStatus check_switch(AstSwitch* switchnode) {
    if (switchnode->initialization != NULL) CHECK(statement_chain, &switchnode->initialization);

    CHECK(expression, &switchnode->expr);
    Type* resolved_expr_type = resolve_expression_type(switchnode->expr);

    if (!(switchnode->flags & Ast_Flag_Has_Been_Checked)) {
        if (resolved_expr_type == NULL) YIELD(switchnode->token->pos, "Waiting for expression type to be known.");

        switchnode->switch_kind = Switch_Kind_Integer;
        if (!type_is_integer(switchnode->expr->type) && switchnode->expr->type->kind != Type_Kind_Enum) {
            switchnode->switch_kind = Switch_Kind_Use_Equals;
        }

        switch (switchnode->switch_kind) {
            case Switch_Kind_Integer:
                switchnode->min_case = 0xffffffffffffffff;
                bh_imap_init(&switchnode->case_map, global_heap_allocator, 4);
                break;

            case Switch_Kind_Use_Equals:
                bh_arr_new(global_heap_allocator, switchnode->case_exprs, 4);
                break;

            default: assert(0);
        }
    }
    switchnode->flags |= Ast_Flag_Has_Been_Checked;

    // Should the case block code be checked here?
    // Or should this just exist to resolve macros and expand #unquotes
    // then the cases are consumed into the array or cases, THEN the blocks
    // are actually checked?
    if (switchnode->cases == NULL) {
        CHECK(block, switchnode->case_block);

        bh_arr_new(global_heap_allocator, switchnode->cases, 4);
        if (collect_switch_case_blocks(switchnode, switchnode->case_block) != Check_Success) {
            return Check_Error;
        }

        // This is important, otherwise if this block has to return to symbol resolution.
        switchnode->case_block->statement_idx = 0;
    }

    fori (i, switchnode->yield_return_index, bh_arr_length(switchnode->cases)) {
        AstSwitchCase *sc = switchnode->cases[i];
        CHECK(block, sc->block);

        bh_arr_each(AstTyped *, value, sc->values) {
            CHECK(expression, value);

            if (switchnode->switch_kind == Switch_Kind_Integer && (*value)->kind == Ast_Kind_Range_Literal) {
                AstRangeLiteral* rl = (AstRangeLiteral *) (*value);
                resolve_expression_type(rl->low);
                resolve_expression_type(rl->high);

                if (rl->low->kind != Ast_Kind_NumLit || rl->high->kind != Ast_Kind_NumLit) {
                    ERROR(rl->token->pos, "case statement expected compile time known range.");
                }

                promote_numlit_to_larger((AstNumLit *) rl->low);
                promote_numlit_to_larger((AstNumLit *) rl->high);

                i64 lower = ((AstNumLit *) rl->low)->value.l;
                i64 upper = ((AstNumLit *) rl->high)->value.l;

                // NOTE: This is inclusive!!!!
                fori (case_value, lower, upper + 1) {
                    if (add_case_to_switch_statement(switchnode, case_value, sc->block, rl->token->pos))
                        return Check_Error;
                }

                continue;
            }

            TYPE_CHECK(value, resolved_expr_type) {
                OnyxToken* tkn = sc->block->token;
                if ((*value)->token) tkn = (*value)->token;

                ERROR_(tkn->pos, "Mismatched types in switch-case. Expected '%s', got '%s'.",
                    type_get_name(resolved_expr_type), type_get_name((*value)->type));
            }

            switch (switchnode->switch_kind) {
                case Switch_Kind_Integer: {
                    b32 is_valid;
                    i64 integer_value = get_expression_integer_value(*value, &is_valid);
                    if (!is_valid)
                        ERROR_((*value)->token->pos, "Case statement expected compile time known integer. Got '%s'.", onyx_ast_node_kind_string((*value)->kind));

                    if (add_case_to_switch_statement(switchnode, integer_value, sc->block, sc->block->token->pos))
                        return Check_Error;

                    break;
                }

                case Switch_Kind_Use_Equals: {
                    // Gross
                    b32 found = 0;
                    bh_arr_each(CaseToBlock, ctb, switchnode->case_exprs) {
                        if (ctb->original_value == *value) {
                            CHECK(expression, (AstTyped **) &ctb->comparison);
                            found = 1;
                            break;
                        }
                    }
                    if (found) break;

                    CaseToBlock ctb;
                    ctb.block = sc->block;
                    ctb.original_value = *value;
                    ctb.comparison = make_binary_op(context.ast_alloc, Binary_Op_Equal, switchnode->expr, *value);
                    ctb.comparison->token = (*value)->token;
                    bh_arr_push(switchnode->case_exprs, ctb);

                    CHECK(binaryop, &bh_arr_last(switchnode->case_exprs).comparison);
                    break;
                }
            }
        }

        switchnode->yield_return_index += 1;
    }

    if (switchnode->default_case)
        CHECK(block, switchnode->default_case);

    return 0;
}

CheckStatus check_arguments(Arguments* args) {
    bh_arr_each(AstTyped *, actual, args->values)
        CHECK(expression, actual);

    bh_arr_each(AstNamedValue *, named_value, args->named_values)
        CHECK(expression, &(*named_value)->value);

    return Check_Success;
}

CheckStatus check_argument(AstArgument** parg) {
    CHECK(expression, &(*parg)->value);
    (*parg)->type = (*parg)->value->type;

    return Check_Success;
}

static CheckStatus check_resolve_callee(AstCall* call, AstTyped** effective_callee) {
    if (call->kind == Ast_Kind_Intrinsic_Call) return Check_Success;

    AstTyped* callee = (AstTyped *) strip_aliases((AstNode *) call->callee);
    b32 calling_a_macro = 0;

    if (callee->kind == Ast_Kind_Overloaded_Function) {
        AstTyped* new_callee = find_matching_overload_by_arguments(
            ((AstOverloadedFunction *) callee)->overloads,
            &call->args);

        if (new_callee == NULL) {
            report_unable_to_match_overload(call, ((AstOverloadedFunction *) callee)->overloads);
            return Check_Error;
        }

        if (new_callee == (AstTyped *) &node_that_signals_a_yield) {
            YIELD(call->token->pos, "Waiting for overloaded function option to pass type-checking.");
        }

        callee = new_callee;
    }

    if (callee->kind == Ast_Kind_Macro) {
        calling_a_macro = 1;
        call->callee = callee;

        AstTyped* new_callee = (AstTyped *) macro_resolve_header((AstMacro *) callee, &call->args, call->token, 1);
        if (new_callee == NULL) return Check_Error;
        if (new_callee == (AstTyped *) &node_that_signals_a_yield) {
            YIELD(call->token->pos, "Waiting for macro header to pass type-checking.");
        }

        arguments_remove_baked(&call->args);
        callee = new_callee;

    } else while (callee->kind == Ast_Kind_Polymorphic_Proc) {
        AstTyped* new_callee = (AstTyped *) polymorphic_proc_lookup((AstFunction *) callee, PPLM_By_Arguments, &call->args, call->token);
        if (new_callee == NULL) return Check_Error;
        if (new_callee == (AstTyped *) &node_that_signals_a_yield) {
            YIELD(call->token->pos, "Waiting for polymorphic procedure header to pass type-checking.");
        }

        arguments_remove_baked(&call->args);
        callee = new_callee;
    }

    // NOTE: Build callee's type
    fill_in_type((AstTyped *) callee);
    if (callee->type == NULL) {
        YIELD(call->token->pos, "Trying to resolve function type for callee.");
    }

    if (!calling_a_macro) call->callee = callee;

    if (callee->type->kind != Type_Kind_Function) {
        ERROR_(call->token->pos,
                "Attempting to call something that is not a function, '%b'.",
                callee->token->text, callee->token->length);
    }

    *effective_callee = callee;
    return Check_Success;
}

CheckStatus check_call(AstCall** pcall) {
    // All the things that need to be done when checking a call node.
    //      1. Ensure the callee is not a symbol
    //      2. Check the callee expression (since it could be a variable or a field access, etc)
    //      3. Check all arguments
    //          * Cannot pass overloaded functions (ROBUSTNESS)
    //      4. If callee is an overloaded function, use the argument types to determine which overload is used.
    //      5. If callee is polymorphic, use the arguments type to generate a polymorphic function.
    //      7. Fill in arguments
    //      8. If callee is an intrinsic, turn call into an Intrinsic_Call node
    //      9. Check types of formal and actual params against each other, handling varargs
    AstCall* call = *pcall;

    if (call->kind == Ast_Kind_Call) {
        AstNode* callee = strip_aliases((AstNode *) call->callee);
        if (callee->kind == Ast_Kind_Poly_Struct_Type) {
            *pcall = (AstCall *) convert_call_to_polycall(call);
            CHECK(expression, (AstTyped **) pcall);
            return Check_Success;
        }
    }

    if (call->flags & Ast_Flag_Has_Been_Checked) return Check_Success;

    u32 current_checking_level_store = current_checking_level;
    CHECK(expression, &call->callee);
    CHECK(arguments, &call->args);
    current_checking_level = current_checking_level_store;

    AstFunction* callee=NULL;
    CHECK(resolve_callee, call, (AstTyped **) &callee);

    if (callee->kind == Ast_Kind_Function) {
        if (callee->constraints.constraints != NULL && callee->constraints.constraints_met == 0) {
            YIELD(call->token->pos, "Waiting for constraints to be checked on callee.");
        }
    }

    i32 arg_count = get_argument_buffer_size(&callee->type->Function, &call->args);
    arguments_ensure_length(&call->args, arg_count);

    char* err_msg = NULL;
    fill_in_arguments(&call->args, (AstNode *) callee, &err_msg, 0);
    if (err_msg != NULL) ERROR(call->token->pos, err_msg);

    bh_arr(AstArgument *) arg_arr = (bh_arr(AstArgument *)) call->args.values;
    bh_arr_each(AstArgument *, arg, arg_arr) {
        if (*arg != NULL) continue;

        ERROR(call->token->pos, "Not all arguments were given a value.");
    }

    // HACK HACK HACK
    // :CallSiteIsGross
    bh_arr_each(AstArgument *, arg, arg_arr) {
        AstTyped** arg_value = &(*arg)->value;

        if ((*arg_value)->kind == Ast_Kind_Call_Site) {
            AstCallSite* callsite = (AstCallSite *) ast_clone(context.ast_alloc, *arg_value);
            callsite->callsite_token = call->token;

            // HACK CLEANUP
            OnyxToken* str_token = bh_alloc(context.ast_alloc, sizeof(OnyxToken));
            str_token->text  = bh_strdup(global_heap_allocator, (char *) call->token->pos.filename);
            str_token->length = strlen(call->token->pos.filename);
            str_token->pos = call->token->pos;
            str_token->type = Token_Type_Literal_String;

            AstStrLit* filename = bh_alloc_item(context.ast_alloc, AstStrLit);
            memset(filename, 0, sizeof(AstStrLit));
            filename->kind  = Ast_Kind_StrLit;
            filename->token = str_token;
            filename->data_id = 0;

            add_entities_for_node(NULL, (AstNode *) filename, NULL, NULL);
            callsite->filename = filename;

            callsite->line   = make_int_literal(context.ast_alloc, call->token->pos.line);
            callsite->column = make_int_literal(context.ast_alloc, call->token->pos.column);

            convert_numlit_to_type(callsite->line,   &basic_types[Basic_Kind_U32]);
            convert_numlit_to_type(callsite->column, &basic_types[Basic_Kind_U32]);

            *arg_value = (AstTyped *) callsite;
        }
    }

    // NOTE: If we are calling an intrinsic function, translate the
    // call into an intrinsic call node.
    if (callee->kind == Ast_Kind_Function && callee->is_intrinsic) {
        call->kind = Ast_Kind_Intrinsic_Call;
        call->callee = NULL;

        token_toggle_end(callee->intrinsic_name);
        char* intr_name = callee->intrinsic_name->text;

        i32 index;
        if ((index = shgeti(intrinsic_table, intr_name)) == -1) {
            onyx_report_error(callee->token->pos, Error_Critical, "Intrinsic not supported, '%s'.", intr_name);
            token_toggle_end(callee->intrinsic_name);
            return Check_Error;
        }

        call->intrinsic = intrinsic_table[index].value;

        token_toggle_end(callee->intrinsic_name);
    }

    call->va_kind = VA_Kind_Not_VA;
    call->type = callee->type->Function.return_type;
    if (call->type == &type_auto_return && call->callee->kind != Ast_Kind_Macro) {
        YIELD(call->token->pos, "Waiting for auto-return type to be solved.");
    }

    OnyxError error;
    TypeMatch tm = check_arguments_against_type(&call->args, &callee->type->Function, &call->va_kind,
                                                call->token, get_function_name(callee), &error);
    if (tm == TYPE_MATCH_FAILED) {
        onyx_submit_error(error);
        return Check_Error;
    }

    if (tm == TYPE_MATCH_YIELD) YIELD(call->token->pos, "Waiting on argument type checking.");

    call->flags   |= Ast_Flag_Has_Been_Checked;
    callee->flags |= Ast_Flag_Function_Used;

    if (call->kind == Ast_Kind_Call && call->callee->kind == Ast_Kind_Macro) {
        expand_macro(pcall, callee);
        return Check_Return_To_Symres;
    }

    return Check_Success;
}

static void report_bad_binaryop(AstBinaryOp* binop) {
    onyx_report_error(binop->token->pos, Error_Critical, "Binary operator '%s' not understood for arguments of type '%s' and '%s'.",
            binaryop_string[binop->operation],
            node_get_type_name(binop->left),
            node_get_type_name(binop->right));
}

static AstCall* binaryop_try_operator_overload(AstBinaryOp* binop, AstTyped* third_argument) {
    if (bh_arr_length(operator_overloads[binop->operation]) == 0) return NULL;

    if (binop->overload_args == NULL) {
        binop->overload_args = bh_alloc_item(context.ast_alloc, Arguments);
        bh_arr_new(context.ast_alloc, binop->overload_args->values, 3);
        bh_arr_set_length(binop->overload_args->values, third_argument ? 3 : 2);

        if (binop_is_assignment(binop->operation)) {
            binop->overload_args->values[0] = (AstTyped *) make_address_of(context.ast_alloc, binop->left);

            u32 current_all_checks_are_final = all_checks_are_final;
            all_checks_are_final = 0;
            u32 current_checking_level_store = current_checking_level;
            CheckStatus cs = check_address_of((AstAddressOf **) &binop->overload_args->values[0]);
            current_checking_level = current_checking_level_store;
            all_checks_are_final   = current_all_checks_are_final;

            if (cs == Check_Yield_Macro)      return (AstCall *) &node_that_signals_a_yield;
            if (cs == Check_Error)            return NULL;

            binop->overload_args->values[0] = (AstTyped *) make_argument(context.ast_alloc, binop->overload_args->values[0]);

        } else {
            binop->overload_args->values[0] = (AstTyped *) make_argument(context.ast_alloc, binop->left);
        }


        binop->overload_args->values[1] = (AstTyped *) make_argument(context.ast_alloc, binop->right);
        if (third_argument != NULL) binop->overload_args->values[2] = (AstTyped *) make_argument(context.ast_alloc, third_argument);
    }

    AstTyped* overload = find_matching_overload_by_arguments(operator_overloads[binop->operation], binop->overload_args);
    if (overload == NULL || overload == (AstTyped *) &node_that_signals_a_yield) return (AstCall *) overload;

    AstCall* implicit_call = onyx_ast_node_new(context.ast_alloc, sizeof(AstCall), Ast_Kind_Call);
    implicit_call->token = binop->token;
    implicit_call->callee = overload;
    implicit_call->va_kind = VA_Kind_Not_VA;

    arguments_clone(&implicit_call->args, binop->overload_args);
    return implicit_call;
}


CheckStatus check_binaryop_assignment(AstBinaryOp** pbinop) {
    AstBinaryOp* binop = *pbinop;
    if (current_checking_level == EXPRESSION_LEVEL)
        ERROR(binop->token->pos, "Assignment not valid in expression.");

    if (!is_lval((AstNode *) binop->left))
        ERROR_(binop->left->token->pos,
                "Cannot assign to '%b'.",
                binop->left->token->text, binop->left->token->length);

    if ((binop->left->flags & Ast_Flag_Const) != 0 && binop->left->type != NULL)
        ERROR_(binop->token->pos,
                "Cannot assign to constant '%b'.",
                binop->left->token->text, binop->left->token->length);

    if (binop->operation == Binary_Op_Assign) {
        // NOTE: Raw assignment

        // NOTE: This is the 'type inference' system. Very stupid, but very easy.
        // If a left operand has an unknown type, fill it in with the type of
        // the right hand side.
        if (binop->left->type == NULL) {
            if (binop->left->type_node != NULL && binop->left->entity && binop->left->entity->state <= Entity_State_Check_Types) {
                YIELD(binop->token->pos, "Waiting for type to be constructed on left hand side.");
            }

            // NOTE: There is a subtlety here. You cannot use the result of `resolve_expression_type` directly,
            // as in some cases (especially with macros and polyprocs), the result is not "correct". The result
            // makes them appears as though they are runtime-known values, which they are not. Using the following
            // pattern does prevent this issue.
            resolve_expression_type(binop->right);

            Type* right_type = get_expression_type(binop->right);
            if (right_type == NULL) {
                if (binop->right->entity == NULL || binop->right->entity->state > Entity_State_Check_Types) {
                    ERROR(binop->token->pos, "Could not resolve type of right hand side to infer.");

                } else {
                    YIELD(binop->token->pos, "Trying to resolve try of right hand side.");
                }
            }

            if (right_type->kind == Type_Kind_Compound) {
                AstCompound* lhs = (AstCompound *) binop->left;
                i32 expr_count = right_type->Compound.count;
                if (lhs->kind != Ast_Kind_Compound || bh_arr_length(lhs->exprs) != expr_count) {
                    ERROR_(binop->token->pos, "Expected left hand side to have %d expressions.", expr_count);
                }

                fori (i, 0, expr_count) lhs->exprs[i]->type = right_type->Compound.types[i];

                lhs->type = type_build_compound_type(context.ast_alloc, lhs);

            } else {
                binop->left->type = right_type;
            }
        }

    } else {
        // NOTE: +=, -=, ...
        // NOTE: At this point, it is assumed that operator overloads for +=, -=, etc have been tested.

        BinaryOp operation = -1;
        if      (binop->operation == Binary_Op_Assign_Add)      operation = Binary_Op_Add;
        else if (binop->operation == Binary_Op_Assign_Minus)    operation = Binary_Op_Minus;
        else if (binop->operation == Binary_Op_Assign_Multiply) operation = Binary_Op_Multiply;
        else if (binop->operation == Binary_Op_Assign_Divide)   operation = Binary_Op_Divide;
        else if (binop->operation == Binary_Op_Assign_Modulus)  operation = Binary_Op_Modulus;
        else if (binop->operation == Binary_Op_Assign_And)      operation = Binary_Op_And;
        else if (binop->operation == Binary_Op_Assign_Or)       operation = Binary_Op_Or;
        else if (binop->operation == Binary_Op_Assign_Xor)      operation = Binary_Op_Xor;
        else if (binop->operation == Binary_Op_Assign_Shl)      operation = Binary_Op_Shl;
        else if (binop->operation == Binary_Op_Assign_Shr)      operation = Binary_Op_Shr;
        else if (binop->operation == Binary_Op_Assign_Sar)      operation = Binary_Op_Sar;

        AstBinaryOp* new_right = make_binary_op(context.ast_alloc, operation, binop->left, binop->right);
        binop->right = (AstTyped *) new_right;
        new_right->token = binop->token;
        binop->operation = Binary_Op_Assign;

        CHECK(binaryop, (AstBinaryOp **) &binop->right);
    }

    if (binop->right->type == NULL) {
        if (binop->right->entity != NULL && binop->right->entity->state <= Entity_State_Check_Types) {
            YIELD(binop->token->pos, "Trying to resolve type of right hand side.");
        }
    }

    TYPE_CHECK(&binop->right, binop->left->type) {
        ERROR_(binop->token->pos,
                "Cannot assign value of type '%s' to a '%s'.",
                node_get_type_name(binop->right),
                node_get_type_name(binop->left));
    }

    binop->type = &basic_types[Basic_Kind_Void];

    return Check_Success;
}

static b32 binary_op_is_allowed(BinaryOp operation, Type* type) {
    static const u8 binop_allowed[Binary_Op_Count] = {
        /* Add */             Basic_Flag_Numeric | Basic_Flag_Pointer,
        /* Minus */           Basic_Flag_Numeric | Basic_Flag_Pointer,
        /* Multiply */        Basic_Flag_Numeric,
        /* Divide */          Basic_Flag_Numeric,
        /* Modulus */         Basic_Flag_Integer,

        /* Equal */           Basic_Flag_Equality,
        /* Not_Equal */       Basic_Flag_Equality,
        /* Less */            Basic_Flag_Ordered,
        /* Less_Equal */      Basic_Flag_Ordered,
        /* Greater */         Basic_Flag_Ordered,
        /* Greater_Equal */   Basic_Flag_Ordered,

        /* And */             Basic_Flag_Integer,
        /* Or */              Basic_Flag_Integer,
        /* Xor */             Basic_Flag_Integer,
        /* Shl */             Basic_Flag_Integer,
        /* Shr */             Basic_Flag_Integer,
        /* Sar */             Basic_Flag_Integer,

        /* Bool_And */        Basic_Flag_Boolean,
        /* Bool_Or */         Basic_Flag_Boolean,

        /* Assign_Start */    0,
        /* Assign */          0,
        /* Assign_Add */      0,
        /* Assign_Minus */    0,
        /* Assign_Multiply */ 0,
        /* Assign_Divide */   0,
        /* Assign_Modulus */  0,
        /* Assign_And */      0,
        /* Assign_Or */       0,
        /* Assign_Xor */      0,
        /* Assign_Shl */      0,
        /* Assign_Shr */      0,
        /* Assign_Sar */      0,
        /* Assign_End */      0,

        /* Pipe */            0,
        /* Range */           0,
    };

    enum BasicFlag effective_flags = 0;
    switch (type->kind) {
        case Type_Kind_Basic:    effective_flags = type->Basic.flags;  break;
        case Type_Kind_Pointer:  effective_flags = Basic_Flag_Pointer; break;
        case Type_Kind_Enum:     effective_flags = Basic_Flag_Integer; break;
        case Type_Kind_Function: effective_flags = Basic_Flag_Equality; break;
    }

    return (binop_allowed[operation] & effective_flags) != 0;
}

CheckStatus check_binaryop_compare(AstBinaryOp** pbinop) {
    AstBinaryOp* binop = *pbinop;

    // HACK: Since ^... to rawptr is a one way conversion, strip any pointers
    // away so they can be compared as expected
    Type* ltype = binop->left->type;
    Type* rtype = binop->right->type;

    if (ltype == NULL) YIELD(binop->token->pos, "Waiting for left-type to be known.");
    if (rtype == NULL) YIELD(binop->token->pos, "Waiting for right-type to be known.");

    if (ltype->kind == Type_Kind_Pointer) ltype = &basic_types[Basic_Kind_Rawptr];
    if (rtype->kind == Type_Kind_Pointer) rtype = &basic_types[Basic_Kind_Rawptr];

    if (!types_are_compatible(ltype, rtype)) {
        b32 left_ac  = node_is_auto_cast((AstNode *) binop->left);
        b32 right_ac = node_is_auto_cast((AstNode *) binop->right);
        if (left_ac && right_ac) ERROR(binop->token->pos, "Cannot have auto cast on both sides of binary operator.");

        TYPE_CHECK(&binop->left, rtype) {
            TYPE_CHECK(&binop->right, ltype) {
                ERROR_(binop->token->pos,
                        "Cannot compare '%s' to '%s'.",
                        type_get_name(binop->left->type),
                        type_get_name(binop->right->type));
            }
        }
    }

    if (!binary_op_is_allowed(binop->operation, binop->left->type)) {
        report_bad_binaryop(binop);
        return Check_Error;
    }

    binop->type = &basic_types[Basic_Kind_Bool];
    if (binop->flags & Ast_Flag_Comptime) {
        // NOTE: Not a binary op
        *pbinop = (AstBinaryOp *) ast_reduce(context.ast_alloc, (AstTyped *) binop);
    }

    return Check_Success;
}

CheckStatus check_binaryop_bool(AstBinaryOp** pbinop) {
    AstBinaryOp* binop = *pbinop;

    if (!type_is_bool(binop->left->type) || !type_is_bool(binop->right->type)) {
        report_bad_binaryop(binop);
        return Check_Error;
    }

    binop->type = &basic_types[Basic_Kind_Bool];

    if (binop->flags & Ast_Flag_Comptime) {
        // NOTE: Not a binary op
        *pbinop = (AstBinaryOp *) ast_reduce(context.ast_alloc, (AstTyped *) binop);
    }
    return Check_Success;
}

CheckStatus check_binaryop(AstBinaryOp** pbinop) {
    AstBinaryOp* binop = *pbinop;

    if (binop->flags & Ast_Flag_Has_Been_Checked) return Check_Success;

    if (binop->operation == Binary_Op_Assign && binop->left->kind == Ast_Kind_Subscript && bh_arr_length(operator_overloads[Binary_Op_Subscript_Equals]) > 0) {
        AstSubscript* sub = (AstSubscript *) binop->left;

        if (binop->potential_substitute == NULL) {
            u32 current_checking_level_store = current_checking_level;
            CHECK(expression, &sub->addr);
            CHECK(expression, &sub->expr);
            CHECK(expression, &binop->right);
            current_checking_level = current_checking_level_store;

            AstBinaryOp *op = onyx_ast_node_new(context.ast_alloc, sizeof(AstBinaryOp), Ast_Kind_Binary_Op);
            op->token = binop->token;
            op->operation = Binary_Op_Subscript_Equals;
            op->left  = ((AstSubscript *) binop->left)->addr;
            op->right = ((AstSubscript *) binop->left)->expr;

            binop->potential_substitute = op;
        }

        AstCall* call = binaryop_try_operator_overload(binop->potential_substitute, binop->right);
        if (call == (AstCall *) &node_that_signals_a_yield) YIELD(binop->token->pos, "Waiting on potential operator overload.");
        if (call != NULL) {
            call->next = binop->next;
            *(AstCall **) pbinop = call;

            CHECK(call, (AstCall **) pbinop);
            return Check_Success;
        }

    }

    u32 current_checking_level_store = current_checking_level;
    CHECK(expression, &binop->left);
    CHECK(expression, &binop->right);
    current_checking_level = current_checking_level_store;

    // :UnaryFieldAccessIsGross
    if (binop->left->kind == Ast_Kind_Unary_Field_Access || binop->right->kind == Ast_Kind_Unary_Field_Access) {
        TYPE_CHECK(&binop->left, binop->right->type) {
            TYPE_CHECK(&binop->right, binop->left->type) {
                // TODO: This should report a better error about the Unary_Field_Access not be able to be resolved given whatever type.
                //                                                                        - brendanfh 2021/12/31
                report_bad_binaryop(binop);
                return Check_Error;
            }
        }
    }

    if ((binop->left->flags & Ast_Flag_Comptime) && (binop->right->flags & Ast_Flag_Comptime)) {
        binop->flags |= Ast_Flag_Comptime;
    }

    if (expression_types_must_be_known) {
        if (binop->left->type == NULL || binop->right->type == NULL) {
            ERROR(binop->token->pos, "Internal compiler error: one of the operands types is unknown here.");
        }
    }

    // NOTE: Try operator overloading before checking everything else.
    if ((binop->left->type != NULL && (binop->left->type->kind != Type_Kind_Basic || (binop->left->type->Basic.flags & Basic_Flag_SIMD) != 0))
        || (binop->right->type != NULL && (binop->right->type->kind != Type_Kind_Basic || (binop->right->type->Basic.flags & Basic_Flag_SIMD) != 0))) {
        AstCall *implicit_call = binaryop_try_operator_overload(binop, NULL);

        if (implicit_call == (AstCall *) &node_that_signals_a_yield)
            YIELD(binop->token->pos, "Trying to resolve operator overload.");

        if (implicit_call != NULL) {
            // NOTE: Not a binary op
            implicit_call->next = binop->next;
            *pbinop = (AstBinaryOp *) implicit_call;

            CHECK(call, (AstCall **) pbinop);
            return Check_Success;
        }
    }

    if (binop_is_assignment(binop->operation)) return check_binaryop_assignment(pbinop);

    if (binop->left->type == NULL && binop->left->entity && binop->left->entity->state <= Entity_State_Check_Types) {
        YIELD(binop->left->token->pos, "Waiting for this type to be known");
    }
    if (binop->right->type == NULL && binop->right->entity && binop->right->entity->state <= Entity_State_Check_Types) {
        YIELD(binop->right->token->pos, "Waiting for this type to be known");
    }

    // NOTE: Comparision operators and boolean operators are handled separately.
    if (binop_is_compare(binop->operation))
        return check_binaryop_compare(pbinop);
    if (binop->operation == Binary_Op_Bool_And || binop->operation == Binary_Op_Bool_Or)
        return check_binaryop_bool(pbinop);

    // NOTE: The left side cannot be compound.
    //       The right side always is numeric.
    //       The left side cannot be rawptr.
    if (type_is_compound(binop->left->type))  goto bad_binaryop;
    if (!type_is_numeric(binop->right->type)) goto bad_binaryop;
    if (type_is_rawptr(binop->left->type)) {
        ERROR(binop->token->pos, "Cannot operate on a 'rawptr'. Cast it to a another pointer type first.");
    }

    // NOTE: Handle basic pointer math.
    if (type_is_pointer(binop->left->type)) {
        if (binop->operation != Binary_Op_Add && binop->operation != Binary_Op_Minus) goto bad_binaryop;

        resolve_expression_type(binop->right);
        if (!type_is_integer(binop->right->type)) goto bad_binaryop;

        AstNumLit* numlit = make_int_literal(context.ast_alloc, type_size_of(binop->left->type->Pointer.elem));
        numlit->token = binop->right->token;
        numlit->type = binop->right->type;

        AstBinaryOp* binop_node = make_binary_op(context.ast_alloc, Binary_Op_Multiply, binop->right, (AstTyped *) numlit);
        binop_node->token = binop->token;
        CHECK(binaryop, &binop_node);

        binop->right = (AstTyped *) binop_node;
        binop->type = binop->left->type;
        binop->right->type = binop->left->type;
    }

    if (!types_are_compatible(binop->left->type, binop->right->type)) {
        b32 left_ac  = node_is_auto_cast((AstNode *) binop->left);
        b32 right_ac = node_is_auto_cast((AstNode *) binop->right);
        if (left_ac && right_ac) {
            ERROR(binop->token->pos, "Cannot have auto cast on both sides of binary operator.");
        }

        TYPE_CHECK(&binop->left, binop->right->type) {
            TYPE_CHECK(&binop->right, binop->left->type) {
                ERROR_(binop->token->pos,
                        "Mismatched types for binary operation '%s'. left: '%s', right: '%s'.",
                        binaryop_string[binop->operation],
                        node_get_type_name(binop->left),
                        node_get_type_name(binop->right));
            }
        }
    }

    binop->type = binop->left->type;
    if (!binary_op_is_allowed(binop->operation, binop->type)) goto bad_binaryop;

    // NOTE: Enum flags with '&' result in a boolean value
    if (binop->type->kind == Type_Kind_Enum && binop->type->Enum.is_flags && binop->operation == Binary_Op_And) {
         binop->type = &basic_types[Basic_Kind_Bool];
    }

    if (all_checks_are_final) {
        binop->flags |= Ast_Flag_Has_Been_Checked;

        if (binop->flags & Ast_Flag_Comptime) {
            // NOTE: Not a binary op
            *pbinop = (AstBinaryOp *) ast_reduce(context.ast_alloc, (AstTyped *) binop);
        }
    }

    return Check_Success;

bad_binaryop:
    report_bad_binaryop(binop);

    return Check_Error;
}

CheckStatus check_unaryop(AstUnaryOp** punop) {
    AstUnaryOp* unaryop = *punop;

    CHECK(expression, &unaryop->expr);

    if (unaryop->operation != Unary_Op_Negate) {
        resolve_expression_type(unaryop->expr);
    }

    if (unaryop->operation == Unary_Op_Cast) {
        char* err;
        if (unaryop->type == NULL)
            YIELD(unaryop->token->pos, "Trying to resolve destination type for cast.");

        if (!cast_is_legal(unaryop->expr->type, unaryop->type, &err)) {
            ERROR_(unaryop->token->pos, "Cast Error: %s", err);
        }

    } else {
        unaryop->type = unaryop->expr->type;
    }

    if (unaryop->operation == Unary_Op_Not) {
        if (!type_is_bool(unaryop->expr->type)) {
            ERROR_(unaryop->token->pos,
                    "Bool negation operator expected bool type, got '%s'.",
                    node_get_type_name(unaryop->expr));
        }
    }

    if (unaryop->operation == Unary_Op_Bitwise_Not) {
        if (!type_is_integer(unaryop->expr->type)) {
            ERROR_(unaryop->token->pos,
                    "Bitwise operator expected integer type, got '%s'.",
                    node_get_type_name(unaryop->expr));
        }
    }

    if (unaryop->expr->flags & Ast_Flag_Comptime) {
        unaryop->flags |= Ast_Flag_Comptime;
        // NOTE: Not a unary op
        *punop = (AstUnaryOp *) ast_reduce(context.ast_alloc, (AstTyped *) unaryop);
    }

    return Check_Success;
}

CheckStatus check_struct_literal(AstStructLiteral* sl) {

    if (sl->type == NULL) {
        // NOTE: This is used for automatically typed struct literals. If there is no provided
        // type for the struct literal, assume that it is passes successfully. When it is used
        // elsewhere, it will be added as an expression entity that will be processed once the
        // stnode is filled out.
        if (sl->stnode == NULL) {
            CHECK(arguments, &sl->args);

            return Check_Success;
        }

        CHECK(expression, &sl->stnode);
        if (!node_is_type((AstNode *) sl->stnode)) {
            ERROR(sl->token->pos, "Type used for struct literal is not a type.");
        }

        sl->type = type_build_from_ast(context.ast_alloc, (AstType *) sl->stnode);
        if (sl->type == NULL)
            YIELD(sl->token->pos, "Trying to resolve type of struct literal.");
    }

    if (!type_is_structlike_strict(sl->type)) {
        //
        // If there are no given arguments to a structure literal, it is treated as a 'zero-value',
        // and can be used to create a completely zeroed value of any type.
        if (bh_arr_length(sl->args.values) == 0 && bh_arr_length(sl->args.named_values) == 0) {
            AstZeroValue *zv = make_zero_value(context.ast_alloc, sl->token, sl->type);
            bh_arr_push(sl->args.values, (AstTyped *) zv);

            sl->flags |= Ast_Flag_Has_Been_Checked;
            return Check_Success;
        }

        if ((sl->flags & Ast_Flag_Has_Been_Checked) != 0) {
            assert(sl->args.values);
            assert(sl->args.values[0]);
            assert(sl->args.values[0]->kind == Ast_Kind_Zero_Value);
            return Check_Success;
        }

        //
        // Otherwise, it is not possible to construct the type if it is not a structure.
        ERROR_(sl->token->pos,
                "'%s' is not constructable using a struct literal.",
                type_get_name(sl->type));
    }

    i32 mem_count = type_structlike_mem_count(sl->type);
    arguments_ensure_length(&sl->args, mem_count);

    // :Idempotency
    if ((sl->flags & Ast_Flag_Has_Been_Checked) == 0) {
        char* err_msg = NULL;
        if (!fill_in_arguments(&sl->args, (AstNode *) sl, &err_msg, 1)) {
            onyx_report_error(sl->token->pos, Error_Critical, err_msg);

            bh_arr_each(AstTyped *, value, sl->args.values) {
                if (*value == NULL) {
                    i32 member_idx = value - sl->args.values; // Pointer subtraction hack
                    StructMember smem;
                    type_lookup_member_by_idx(sl->type, member_idx, &smem);

                    onyx_report_error(sl->token->pos, Error_Critical,
                        "Value not given for %d%s member, '%s', for type '%s'.",
                        member_idx + 1, bh_num_suffix(member_idx + 1),
                        smem.name, type_get_name(sl->type));
                }
            }

            return Check_Error;
        }
    }
    sl->flags |= Ast_Flag_Has_Been_Checked;

    AstTyped** actual = sl->args.values;
    StructMember smem;

    // BUG: There are problems setting the comptime flag this late in the checking because
    // if the struct literal was type inferred, then the literal won't be correctly determined
    // to be comptime on the first pass, which is needed for top level expressions.
    sl->flags |= Ast_Flag_Comptime;

    fori (i, 0, mem_count) {
        // NOTE: Not checking the return on this function because
        // this for loop is bounded by the number of members in the
        // type.
        type_lookup_member_by_idx(sl->type, i, &smem);
        Type* formal = smem.type;

        CHECK(expression, actual);
        if ((*actual)->type == NULL && (*actual)->entity != NULL && (*actual)->entity->state <= Entity_State_Check_Types) {
            YIELD((*actual)->token->pos, "Trying to resolve type of expression for member.");
        }

        TYPE_CHECK(actual, formal) {
            ERROR_(sl->token->pos,
                    "Mismatched types for %d%s member named '%s', expected '%s', got '%s'.",
                    i + 1, bh_num_suffix(i + 1),
                    smem.name,
                    type_get_name(formal),
                    node_get_type_name(*actual));
        }

        sl->flags &= ((*actual)->flags & Ast_Flag_Comptime) | (sl->flags &~ Ast_Flag_Comptime);
        actual++;
    }

    return Check_Success;
}

CheckStatus check_array_literal(AstArrayLiteral* al) {
    // :Idempotency
    if ((al->flags & Ast_Flag_Array_Literal_Typed) == 0) {
        if (al->atnode == NULL) return Check_Success;
            // YIELD(al->token->pos, "Waiting for array literal type to be known.");

        CHECK(expression, &al->atnode);
        if (!node_is_type((AstNode *) al->atnode))
            ERROR(al->token->pos, "Array type is not a type.");

        al->type = type_build_from_ast(context.ast_alloc, (AstType *) al->atnode);
        if (al->type == NULL)
            YIELD(al->token->pos, "Trying to resolve type of array literal.");

        al->type = type_make_array(context.ast_alloc, al->type, bh_arr_length(al->values));
        if (al->type == NULL || al->type->kind != Type_Kind_Array)
            ERROR(al->token->pos, "Expected array type for array literal. This is a compiler bug.");

        al->flags |= Ast_Flag_Array_Literal_Typed;
    }

    if (al->type->Array.count != (u32) bh_arr_length(al->values)) {
        ERROR_(al->token->pos, "Wrong array size (%d) for number of values (%d).",
            al->type->Array.count, bh_arr_length(al->values));
    }

    al->flags |= Ast_Flag_Comptime;
    assert(al->type->kind == Type_Kind_Array);

    Type* elem_type = al->type->Array.elem;
    bh_arr_each(AstTyped *, expr, al->values) {
        CHECK(expression, expr);

        // HACK HACK HACK
        if ((*expr)->type == NULL &&
            (*expr)->entity != NULL &&
            (*expr)->entity->state <= Entity_State_Check_Types) {
            YIELD_(al->token->pos, "Trying to resolve type of %d%s element of array literal.", expr - al->values, bh_num_suffix(expr - al->values));
        }

        al->flags &= ((*expr)->flags & Ast_Flag_Comptime) | (al->flags &~ Ast_Flag_Comptime);

        TYPE_CHECK(expr, elem_type) {
            ERROR_((*expr)->token->pos, "Mismatched types for value of in array, expected '%s', got '%s'.",
                type_get_name(elem_type),
                node_get_type_name(*expr));
        }
    }

    return Check_Success;
}

CheckStatus check_range_literal(AstRangeLiteral** prange) {
    AstRangeLiteral* range = *prange;
    if (range->flags & Ast_Flag_Has_Been_Checked) return Check_Success;

    CHECK(expression, &range->low);
    CHECK(expression, &range->high);

    builtin_range_type_type = type_build_from_ast(context.ast_alloc, builtin_range_type);
    if (builtin_range_type_type == NULL) YIELD(range->token->pos, "Waiting for 'range' structure to be built.");

    Type* expected_range_type = builtin_range_type_type;
    StructMember smem;

    type_lookup_member(expected_range_type, "low", &smem);
    TYPE_CHECK(&range->low, smem.type) {
        ERROR_(range->token->pos,
            "Expected left side of range to be a 32-bit integer, got '%s'.",
            node_get_type_name(range->low));
    }

    type_lookup_member(expected_range_type, "high", &smem);
    TYPE_CHECK(&range->high, smem.type) {
        ERROR_(range->token->pos,
            "Expected right side of range to be a 32-bit integer, got '%s'.",
            node_get_type_name(range->high));
    }

    if (range->step == NULL) {
        type_lookup_member(expected_range_type, "step", &smem);
        assert(smem.initial_value != NULL);
        CHECK(expression, smem.initial_value);

        range->step = *smem.initial_value;
    }

    range->flags |= Ast_Flag_Has_Been_Checked;
    return Check_Success;
}

CheckStatus check_compound(AstCompound* compound) {
    bh_arr_each(AstTyped *, expr, compound->exprs) {
        CHECK(expression, expr);
    }

    compound->type = type_build_compound_type(context.ast_alloc, compound);
    return Check_Success;
}

CheckStatus check_if_expression(AstIfExpression* if_expr) {
    CHECK(expression, &if_expr->cond);
    CHECK(expression, &if_expr->true_expr);
    CHECK(expression, &if_expr->false_expr);

    TYPE_CHECK(&if_expr->cond, &basic_types[Basic_Kind_Bool]) {
        ERROR_(if_expr->token->pos, "If-expression expected boolean for condition, got '%s'.",
            type_get_name(if_expr->cond->type));
    }

    resolve_expression_type((AstTyped *) if_expr);

    if (!types_are_compatible(if_expr->true_expr->type, if_expr->false_expr->type)) {
        ERROR_(if_expr->token->pos, "Mismatched types for if-expression, left side is '%s', and right side is '%s'.",
            type_get_name(if_expr->true_expr->type), type_get_name(if_expr->false_expr->type));
    }

    return Check_Success;
}

CheckStatus check_do_block(AstDoBlock** pdoblock) {
    AstDoBlock* doblock = *pdoblock;
    if (doblock->flags & Ast_Flag_Has_Been_Checked) return Check_Success;

    fill_in_type((AstTyped *) doblock);

    Type** old_expected_return_type = expected_return_type;
    expected_return_type = &doblock->type;

    doblock->block->rules = Block_Rule_Do_Block;
    CHECK(block, doblock->block);

    if (doblock->type == &type_auto_return) doblock->type = &basic_types[Basic_Kind_Void];

    expected_return_type = old_expected_return_type;
    doblock->flags |= Ast_Flag_Has_Been_Checked;
    return Check_Success;
}

CheckStatus check_address_of(AstAddressOf** paof) {
    AstAddressOf* aof = *paof;

    AstTyped* expr = (AstTyped *) strip_aliases((AstNode *) aof->expr);
    if (expr->kind == Ast_Kind_Subscript && bh_arr_length(operator_overloads[Binary_Op_Ptr_Subscript]) > 0) {
        if (aof->potential_substitute == NULL) {
            CHECK(expression, &((AstSubscript *) expr)->addr);
            CHECK(expression, &((AstSubscript *) expr)->expr);

            AstBinaryOp *op = onyx_ast_node_new(context.ast_alloc, sizeof(AstBinaryOp), Ast_Kind_Binary_Op);
            op->operation = Binary_Op_Ptr_Subscript;
            op->left  = ((AstSubscript *) expr)->addr;
            op->right = ((AstSubscript *) expr)->expr;
            op->token = aof->token;

            aof->potential_substitute = op;
        }

        AstCall* call = binaryop_try_operator_overload(aof->potential_substitute, NULL);
        if (call == (AstCall *) &node_that_signals_a_yield) YIELD(aof->token->pos, "Waiting for operator overload to possibly resolve.");
        if (call != NULL) {
            call->next = aof->next;
            *(AstCall **) paof = call;

            CHECK(call, (AstCall **) paof);
            return Check_Success;
        }
    }

    CHECK(expression, &aof->expr);
    if (node_is_addressable_literal((AstNode *) aof->expr)) {
        resolve_expression_type(aof->expr);
    }
    
    if (aof->expr->type == NULL) {
        YIELD(aof->token->pos, "Trying to resolve type of expression to take a reference.");
    }

    expr = (AstTyped *) strip_aliases((AstNode *) aof->expr);
    if (node_is_type((AstNode *) expr)) {
        AstPointerType *pt = onyx_ast_node_new(context.ast_alloc, sizeof(AstPointerType), Ast_Kind_Pointer_Type);
        pt->token     = aof->token;
        pt->elem      = (AstType *) expr;
        pt->__unused  = aof->next;
        *paof         = (AstAddressOf *) pt;
        CHECK(type, (AstType **) &pt);
        return Check_Success;
    }

    if ((expr->kind != Ast_Kind_Subscript
            && expr->kind != Ast_Kind_Dereference
            && expr->kind != Ast_Kind_Field_Access
            && expr->kind != Ast_Kind_Memres
            && expr->kind != Ast_Kind_Local
            && expr->kind != Ast_Kind_Constraint_Sentinel
            && !node_is_addressable_literal((AstNode *) expr))
            || (expr->flags & Ast_Flag_Cannot_Take_Addr) != 0) {

        if (aof->can_be_removed) {
            *(AstTyped **) paof = aof->expr;
            return Check_Yield_Macro;
        }

        ERROR_(aof->token->pos, "Cannot take the address of something that is not an l-value. %s", onyx_ast_node_kind_string(expr->kind));
    }

    expr->flags |= Ast_Flag_Address_Taken;

    aof->type = type_make_pointer(context.ast_alloc, expr->type);

    return Check_Success;
}

CheckStatus check_dereference(AstDereference* deref) {
    CHECK(expression, &deref->expr);

    if (!type_is_pointer(deref->expr->type))
        ERROR(deref->token->pos, "Cannot dereference non-pointer value.");

    if (deref->expr->type == basic_type_rawptr.basic_type)
        ERROR(deref->token->pos, "Cannot dereference 'rawptr'. Cast to another pointer type first.");

    deref->type = deref->expr->type->Pointer.elem;

    return Check_Success;
}

CheckStatus check_subscript(AstSubscript** psub) {
    AstSubscript* sub = *psub;
    CHECK(expression, &sub->addr);
    CHECK(expression, &sub->expr);

    if (sub->addr->type == NULL) YIELD(sub->token->pos, "Waiting to know type of left-hand side of subscript.");

    // NOTE: Try operator overloading before checking everything else.
    if (sub->expr->type != NULL &&
        (sub->addr->type->kind != Type_Kind_Basic || sub->expr->type->kind != Type_Kind_Basic)) {
        // AstSubscript is the same as AstBinaryOp for the first sizeof(AstBinaryOp) bytes
        AstBinaryOp* binop = (AstBinaryOp *) sub;
        AstCall *implicit_call = binaryop_try_operator_overload(binop, NULL);

        if (implicit_call == (AstCall *) &node_that_signals_a_yield)
            YIELD(sub->token->pos, "Trying to resolve operator overload.");

        if (implicit_call != NULL) {
            // NOTE: Not an array access
            implicit_call->next = sub->next;
            *psub = (AstSubscript *) implicit_call;

            CHECK(call, (AstCall **) psub);
            return Check_Success;
        }
    }

    if (!type_is_array_accessible(sub->addr->type)) {
        report_bad_binaryop((AstBinaryOp *) sub);
        return Check_Error;
    }

    if (sub->addr->type->kind == Type_Kind_Slice || sub->addr->type->kind == Type_Kind_DynArray || sub->addr->type->kind == Type_Kind_VarArgs) {
        // If we are accessing on a slice or a dynamic array, implicitly add a field access for the data member
        StructMember smem;
        type_lookup_member(sub->addr->type, "data", &smem);

        AstFieldAccess* fa = make_field_access(context.ast_alloc, sub->addr, "data");
        fa->type   = smem.type;
        fa->offset = smem.offset;
        fa->idx    = smem.idx;

        sub->addr = (AstTyped *) fa;
    }

    if (types_are_compatible(sub->expr->type, builtin_range_type_type)) {
        Type *of = type_get_contained_type(sub->addr->type);
        if (of == NULL) {
            // FIXME: Slice creation should be allowed for slice types and dynamic array types, like it
            // is below, but this code doesn't look at that.
            report_bad_binaryop((AstBinaryOp *) sub);
            ERROR(sub->token->pos, "Invalid type for left of slice creation.");
        }

        sub->kind = Ast_Kind_Slice;
        sub->type = type_make_slice(context.ast_alloc, of);
        sub->elem_size = type_size_of(of);

        return Check_Success;
    }

    resolve_expression_type(sub->expr);
    if (!type_is_small_integer(sub->expr->type)) {
        report_bad_binaryop((AstBinaryOp *) sub);
        ERROR_(sub->token->pos, "Expected small integer type for index, got '%s'.", node_get_type_name(sub->expr));
    }

    sub->type = type_get_contained_type(sub->addr->type);
    if (sub->type == NULL) {
        report_bad_binaryop((AstBinaryOp *) sub);
        ERROR(sub->token->pos, "Invalid type for left of array access.");
    }

    sub->elem_size = type_size_of(sub->type);
    return Check_Success;
}

CheckStatus check_field_access(AstFieldAccess** pfield) {
    AstFieldAccess* field = *pfield;
    if (field->flags & Ast_Flag_Has_Been_Checked) return Check_Success;

    CHECK(expression, &field->expr);
    if (field->expr->type == NULL) {
        YIELD(field->token->pos, "Trying to resolve type of source expression.");
    }

    if (!type_is_structlike(field->expr->type)) {
        ERROR_(field->token->pos,
            "Cannot access field '%b' on '%s'. Type is not a struct.",
            field->token->text,
            field->token->length,
            node_get_type_name(field->expr));
    }

    // Optimization for (*foo).member.
    if (field->expr->kind == Ast_Kind_Dereference) {
        field->expr = ((AstDereference *) field->expr)->expr;
    }

    if (field->token != NULL && field->field == NULL) {
        token_toggle_end(field->token);
        field->field = bh_strdup(context.ast_alloc, field->token->text);
        token_toggle_end(field->token);
    }

    if (field->expr->type->kind == Type_Kind_Struct) {
        if (field->expr->type->Struct.status != SPS_Uses_Done) {
            YIELD(field->token->pos, "Waiting for struct type to be completed before looking up members.");
        }
    }

    StructMember smem;
    if (!type_lookup_member(field->expr->type, field->field, &smem)) {
        if (field->expr->type->kind == Type_Kind_Array) {
            if (!strcmp(field->field, "count")) {
                *pfield = (AstFieldAccess *) make_int_literal(context.ast_alloc, field->expr->type->Array.count);
                return Check_Success;
            }
        }

        AstNode* n = try_symbol_raw_resolve_from_type(field->expr->type, field->field);

        AstType* type_node = field->expr->type->ast_type;
        if (!n) n = try_symbol_raw_resolve_from_node((AstNode *) type_node, field->field);

        if (n) {
            *pfield = (AstFieldAccess *) n;
            return Check_Success;
        }

        char* closest = find_closest_symbol_in_node((AstNode *) type_node, field->field);
        if (closest) {
            ERROR_(field->token->pos, "Field '%s' does not exists on '%s'. Did you mean '%s'?", field->field, node_get_type_name(field->expr), closest);
        } else {
            ERROR_(field->token->pos, "Field '%s' does not exists on '%s'.", field->field, node_get_type_name(field->expr));
        }
    }

    // NOTE: If this member was included into the structure through a "use x: ^T" kind of statement,
    // then we have to insert a intermediate field access in order to access the correct member.
    if (smem.use_through_pointer_index >= 0) {
        StructMember containing_member;
        assert(type_lookup_member_by_idx(field->expr->type, smem.use_through_pointer_index, &containing_member));

        AstFieldAccess *new_access = onyx_ast_node_new(context.ast_alloc, sizeof(AstFieldAccess), Ast_Kind_Field_Access);
        new_access->token = field->token;
        new_access->offset = containing_member.offset;
        new_access->idx = containing_member.idx;
        new_access->type = containing_member.type;
        new_access->expr = field->expr;
        new_access->flags |= Ast_Flag_Has_Been_Checked;

        field->expr = (AstTyped *) new_access;
    }

    field->offset = smem.offset;
    field->idx = smem.idx;
    field->type = smem.type;
    field->flags |= Ast_Flag_Has_Been_Checked;
    return Check_Success;
}

CheckStatus check_method_call(AstBinaryOp** pmcall) {
    AstBinaryOp* mcall = *pmcall;
    CHECK(expression, &mcall->left);
    if (mcall->left->type == NULL) YIELD(mcall->token->pos, "Trying to resolve type of left hand side.");

    AstTyped* implicit_argument = mcall->left;

    // Symbol resolution should have ensured that this is call node.
    AstCall* call_node = (AstCall *) mcall->right;
    assert(call_node->kind == Ast_Kind_Call);

    // :Idempotency
    if ((mcall->flags & Ast_Flag_Has_Been_Checked) == 0) {
        // Implicitly take the address of the value if it is not already a pointer type.
        // This could be weird to think about semantically so some testing with real code
        // would be good.                                      - brendanfh 2020/02/05
        if (implicit_argument->type->kind != Type_Kind_Pointer) {
            AstAddressOf *address_of = make_address_of(context.ast_alloc, implicit_argument);
            address_of->can_be_removed = 1;
            implicit_argument = (AstTyped *) address_of;
        }

        implicit_argument = (AstTyped *) make_argument(context.ast_alloc, implicit_argument);

        bh_arr_insertn(call_node->args.values, 0, 1);
        call_node->args.values[0] = implicit_argument;

        *pmcall = (AstBinaryOp *) mcall->right;
        mcall->right->next = mcall->next;
    }
    mcall->flags |= Ast_Flag_Has_Been_Checked;

    CHECK(call, (AstCall **) pmcall);
    return Check_Success;
}

CheckStatus check_size_of(AstSizeOf* so) {
    CHECK(type, &so->so_ast_type);

    so->so_type = type_build_from_ast(context.ast_alloc, so->so_ast_type);
    if (so->so_type == NULL)
        YIELD(so->token->pos, "Trying to resolve type to take the size of.");

    so->size = type_size_of(so->so_type);
    so->flags |= Ast_Flag_Comptime;

    return Check_Success;
}

CheckStatus check_align_of(AstAlignOf* ao) {
    CHECK(type, &ao->ao_ast_type);

    ao->ao_type = type_build_from_ast(context.ast_alloc, ao->ao_ast_type);
    if (ao->ao_type == NULL)
        YIELD(ao->token->pos, "Trying to resolve type to take the alignment of.");

    ao->alignment = type_alignment_of(ao->ao_type);
    ao->flags |= Ast_Flag_Comptime;

    return Check_Success;
}

CheckStatus check_expression(AstTyped** pexpr) {
    AstTyped* expr = *pexpr;
    if (expr->kind > Ast_Kind_Type_Start && expr->kind < Ast_Kind_Type_End) {
        // This is to ensure that the type will exist when compiling. For example, a poly-call type
        // would have to wait for the entity to pass through, which the code generation does not know
        // about.
        CHECK(type, (AstType **) pexpr);
        expr = *pexpr;

        // Don't try to construct a polystruct ahead of time because you can't.
        if (expr->kind != Ast_Kind_Poly_Struct_Type) {
            if (type_build_from_ast(context.ast_alloc, (AstType*) expr) == NULL) {
                YIELD(expr->token->pos, "Trying to construct type.");
            }
        } else {
            type_build_from_ast(context.ast_alloc, (AstType*) expr);
        }

        expr->type = &basic_types[Basic_Kind_Type_Index];
        return Check_Success;
    }

    if (expr->kind == Ast_Kind_Polymorphic_Proc) {
        // polymorphic procedures do not need to be checked. Their concrete instantiations
        // will be checked when they are created.
        return Check_Success;
    }

    if (expr->kind == Ast_Kind_Macro) {
        return Check_Success;
    }

    if (expr->kind == Ast_Kind_Directive_Init) {
        ERROR(expr->token->pos, "#init declarations are not in normal expressions, only in #after clauses.");
    }

    fill_in_type(expr);
    current_checking_level = EXPRESSION_LEVEL;

    CheckStatus retval = Check_Success;
    switch (expr->kind) {
        case Ast_Kind_Binary_Op: retval = check_binaryop((AstBinaryOp **) pexpr); break;
        case Ast_Kind_Unary_Op:  retval = check_unaryop((AstUnaryOp **) pexpr); break;

        case Ast_Kind_Intrinsic_Call:
        case Ast_Kind_Call:     retval = check_call((AstCall **) pexpr); break;
        case Ast_Kind_Argument: retval = check_argument((AstArgument **) pexpr); break;
        case Ast_Kind_Block:    retval = check_block((AstBlock *) expr); break;

        case Ast_Kind_Symbol:
            YIELD_(expr->token->pos, "Waiting to resolve symbol, '%b'.", expr->token->text, expr->token->length);
            break;

        case Ast_Kind_Param:
            if (expr->type == NULL) {
                YIELD(expr->token->pos, "Waiting on parameter type.");
            }
            break;

        case Ast_Kind_Local: break;

        case Ast_Kind_Address_Of:    retval = check_address_of((AstAddressOf **) pexpr); break;
        case Ast_Kind_Dereference:   retval = check_dereference((AstDereference *) expr); break;
        case Ast_Kind_Slice:
        case Ast_Kind_Subscript:     retval = check_subscript((AstSubscript **) pexpr); break;
        case Ast_Kind_Field_Access:  retval = check_field_access((AstFieldAccess **) pexpr); break;
        case Ast_Kind_Method_Call:   retval = check_method_call((AstBinaryOp **) pexpr); break;
        case Ast_Kind_Size_Of:       retval = check_size_of((AstSizeOf *) expr); break;
        case Ast_Kind_Align_Of:      retval = check_align_of((AstAlignOf *) expr); break;
        case Ast_Kind_Range_Literal: retval = check_range_literal((AstRangeLiteral **) pexpr); break;

        case Ast_Kind_Global:
            if (expr->type == NULL) {
                onyx_report_error(expr->token->pos, Error_Critical, "Global with unknown type.");
                retval = Check_Error;
            }
            break;

        case Ast_Kind_NumLit:
            assert(expr->type != NULL);
            break;

        case Ast_Kind_Struct_Literal:
            retval = check_struct_literal((AstStructLiteral *) expr);
            break;

        case Ast_Kind_Array_Literal:
            retval = check_array_literal((AstArrayLiteral *) expr);
            break;

        case Ast_Kind_Function:
            // NOTE: Will need something like this at some point
            // AstFunction* func = (AstFunction *) expr;
            // bh_arr_each(AstParam, param, func->params) {
            //     if (param->default_value != NULL) {
            //         onyx_message_add(Msg_Type_Literal,
            //                 func->token->pos,
            //                 "cannot use functions with default parameters in this way");
            //         retval = 1;
            //         break;
            //     }
            // }
            if (expr->type == NULL)
                YIELD(expr->token->pos, "Waiting for function type to be resolved.");

            expr->flags |= Ast_Flag_Function_Used;
            break;

        case Ast_Kind_Directive_Solidify:
            CHECK(directive_solidify, (AstDirectiveSolidify **) pexpr);
            break;

        case Ast_Kind_Directive_Defined:
            *pexpr = (AstTyped *) make_bool_literal(context.ast_alloc, ((AstDirectiveDefined *) expr)->is_defined);
            fill_in_type(*pexpr);
            break;

        case Ast_Kind_Compound:
            CHECK(compound, (AstCompound *) expr);
            break;

        case Ast_Kind_Call_Site:
            // NOTE: This has to be set here because if it were to be set in the parser,
            // builtin_callsite_type wouldn't be known when parsing the builtin.onyx file.
            expr->type_node = builtin_callsite_type;
            break;

        case Ast_Kind_If_Expression:
            CHECK(if_expression, (AstIfExpression *) expr);
            break;

        case Ast_Kind_Alias:
            CHECK(expression, &((AstAlias *) expr)->alias);
            expr->flags |= (((AstAlias *) expr)->alias->flags & Ast_Flag_Comptime);
            expr->type = ((AstAlias *) expr)->alias->type;
            break;

        case Ast_Kind_Directive_Insert:
            retval = check_insert_directive((AstDirectiveInsert **) pexpr);
            break;

        case Ast_Kind_Code_Block:
            expr->flags |= Ast_Flag_Comptime;
            fill_in_type(expr);
            break;

        case Ast_Kind_Do_Block:
            retval = check_do_block((AstDoBlock **) pexpr);
            break;

        case Ast_Kind_Memres:
            if (expr->type == NULL) YIELD(expr->token->pos, "Waiting to know globals type.");
            break;

        case Ast_Kind_StrLit: break;
        case Ast_Kind_File_Contents: break;
        case Ast_Kind_Overloaded_Function: break;
        case Ast_Kind_Enum_Value: break;
        case Ast_Kind_Polymorphic_Proc: break;
        case Ast_Kind_Package: break;
        case Ast_Kind_Error: break;
        case Ast_Kind_Unary_Field_Access: break;
        case Ast_Kind_Constraint_Sentinel: break;
        case Ast_Kind_Switch_Case: break;
        case Ast_Kind_Foreign_Block: break;
        case Ast_Kind_Zero_Value: break;

        default:
            retval = Check_Error;
            onyx_report_error(expr->token->pos, Error_Critical, "UNEXPECTED INTERNAL COMPILER ERROR");
            DEBUG_HERE;
            break;
    }

    return retval;
}

CheckStatus check_global(AstGlobal* global) {
    fill_in_type((AstTyped *) global);

    if (global->type == NULL) {
        YIELD(global->token->pos, "Trying to resolve type for global.");
    }

    return Check_Success;
}

CheckStatus check_insert_directive(AstDirectiveInsert** pinsert) {
    AstDirectiveInsert* insert = *pinsert;
    if (insert->flags & Ast_Flag_Has_Been_Checked) return Check_Success;

    CHECK(expression, &insert->code_expr);
    if (insert->code_expr->type == NULL) {
        if (insert->code_expr->entity && insert->code_expr->entity->state >= Entity_State_Code_Gen) {
            ERROR(insert->token->pos, "Expected expression of type 'Code'.");
        }

        // Bad wording for the message.
        YIELD(insert->token->pos, "Waiting for resolution to code expression type.");
    }

    Type* code_type = type_build_from_ast(context.ast_alloc, builtin_code_type);

    TYPE_CHECK(&insert->code_expr, code_type) {
        ERROR_(insert->token->pos, "#unquote expected a value of type 'Code', got '%s'.",
            type_get_name(insert->code_expr->type));
    }

    AstCodeBlock* code_block = (AstCodeBlock *) insert->code_expr;
    code_block = (AstCodeBlock *) strip_aliases((AstNode *) code_block);

    assert(code_block->kind == Ast_Kind_Code_Block);

    AstNode* cloned_block = ast_clone(context.ast_alloc, code_block->code);
    cloned_block->next = insert->next;
    *(AstNode **) pinsert = cloned_block;

    insert->flags |= Ast_Flag_Has_Been_Checked;

    return Check_Return_To_Symres;
}

CheckStatus check_directive_solidify(AstDirectiveSolidify** psolid) {
    AstDirectiveSolidify* solid = *psolid;

    bh_arr_each(AstPolySolution, sln, solid->known_polyvars) {
        CHECK(expression, &sln->value);

        if (node_is_type((AstNode *) sln->value)) {
            sln->type = type_build_from_ast(context.ast_alloc, sln->ast_type);
            sln->kind = PSK_Type;
        } else {
            sln->kind = PSK_Value;
        }
    }

    solid->resolved_proc = polymorphic_proc_try_solidify(solid->poly_proc, solid->known_polyvars, solid->token);
    if (solid->resolved_proc == (AstNode *) &node_that_signals_a_yield) {
        solid->resolved_proc = NULL;
        YIELD(solid->token->pos, "Waiting for partially solidified procedure.");
    }

    // NOTE: Not a DirectiveSolidify.
    *psolid = (AstDirectiveSolidify *) solid->resolved_proc;

    return Check_Success;
}

CheckStatus check_remove_directive(AstDirectiveRemove *remove) {
    if (!inside_for_iterator) {
        ERROR(remove->token->pos, "#remove is only allowed in the body of a for-loop over an iterator.");
    }

    return Check_Success;
}

CheckStatus check_statement(AstNode** pstmt) {
    AstNode* stmt = *pstmt;

    current_checking_level = STATEMENT_LEVEL;

    switch (stmt->kind) {
        case Ast_Kind_Jump:       return Check_Success;

        case Ast_Kind_Return:     return check_return((AstReturn *) stmt);
        case Ast_Kind_If:         return check_if((AstIfWhile *) stmt);
        case Ast_Kind_Static_If:  return check_if((AstIfWhile *) stmt);
        case Ast_Kind_While:      return check_while((AstIfWhile *) stmt);
        case Ast_Kind_For:        return check_for((AstFor *) stmt);
        case Ast_Kind_Switch:     return check_switch((AstSwitch *) stmt);
        case Ast_Kind_Block:      return check_block((AstBlock *) stmt);
        case Ast_Kind_Defer:      return check_statement(&((AstDefer *) stmt)->stmt);
        case Ast_Kind_Directive_Remove: return check_remove_directive((AstDirectiveRemove *) stmt);
        case Ast_Kind_Call: {
            CHECK(call, (AstCall **) pstmt);
            (*pstmt)->flags |= Ast_Flag_Expr_Ignored;
            return Check_Success;
        }

        case Ast_Kind_Binary_Op:
            CHECK(binaryop, (AstBinaryOp **) pstmt);
            (*pstmt)->flags |= Ast_Flag_Expr_Ignored;
            return Check_Success;

        // NOTE: Local variable declarations used to be removed after the symbol
        // resolution phase because long long ago, all locals needed to be known
        // in a block in order to efficiently allocate enough space and registers
        // for them all. Now with LocalAllocator, this is no longer necessary.
        // Therefore, locals stay in the tree and need to be passed along.
        case Ast_Kind_Local: {
            AstTyped* typed_stmt = (AstTyped *) stmt;
            fill_in_type(typed_stmt);
            if (typed_stmt->type_node != NULL && typed_stmt->type == NULL) {
                CHECK(type, &typed_stmt->type_node);

                if (!node_is_type((AstNode *) typed_stmt->type_node)) {
                    ERROR(stmt->token->pos, "Local's type is not a type.");
                }

                YIELD(typed_stmt->token->pos, "Waiting for local variable's type.");
            }

            if (typed_stmt->next != NULL && typed_stmt->next->kind == Ast_Kind_Binary_Op) {
                AstBinaryOp *next = (AstBinaryOp *) typed_stmt->next;
                if (next->operation == Binary_Op_Assign && next->left == typed_stmt) {
                    typed_stmt->flags |= Ast_Flag_Decl_Followed_By_Init;
                }
            }
            return Check_Success;
        }

        default:
            CHECK(expression, (AstTyped **) pstmt);
            (*pstmt)->flags |= Ast_Flag_Expr_Ignored;
            return Check_Success;
    }
}

CheckStatus check_statement_chain(AstNode** start) {
    while (*start) {
        CHECK(statement, start);
        start = &(*start)->next;
    }

    return Check_Success;
}

CheckStatus check_block(AstBlock* block) {
    // This used to use statement_chain, but since block optimize which statements need to be rechecked,
    // it has to be its own thing.

    AstNode** start = &block->body;
    fori (i, 0, block->statement_idx) {
        start = &(*start)->next;
    }

    while (*start) {
        CheckStatus cs = check_statement(start);
        switch (cs) {
            case Check_Success:
                start = &(*start)->next;
                block->statement_idx++;
                break;

            case Check_Return_To_Symres:
                block->statement_idx = 0;

            default:
                return cs;
        }

    }

    return Check_Success;
}

CheckStatus check_function(AstFunction* func) {
    if (func->flags & Ast_Flag_Has_Been_Checked) return Check_Success;
    if (func->entity_header && func->entity_header->state < Entity_State_Code_Gen)
        YIELD(func->token->pos, "Waiting for procedure header to pass type-checking");

    bh_arr_each(AstTyped *, pexpr, func->tags) {
        CHECK(expression, pexpr);

        if (((*pexpr)->flags & Ast_Flag_Comptime) == 0) {
            ERROR((*pexpr)->token->pos, "#tag expressions should be compile time known.");
        }
    }

    inside_for_iterator = 0;
    expected_return_type = &func->type->Function.return_type;
    if (func->body) {
        CheckStatus status = check_block(func->body);
        if (status == Check_Error && func->generated_from && context.cycle_detected == 0)
            ERROR(func->generated_from->pos, "Error in polymorphic procedure generated from this location.");

        if (status != Check_Success) {
            expected_return_type = NULL;
            return status;
        }
    }

    if (*expected_return_type == &type_auto_return) {
        *expected_return_type = &basic_types[Basic_Kind_Void];
    }

    func->flags |= Ast_Flag_Has_Been_Checked;
    return Check_Success;
}

CheckStatus check_overloaded_function(AstOverloadedFunction* func) {
    b32 done = 1;

    bh_imap all_overloads;
    bh_imap_init(&all_overloads, global_heap_allocator, 4);
    build_all_overload_options(func->overloads, &all_overloads);

    bh_arr_each(bh__imap_entry, entry, all_overloads.entries) {
        AstTyped* node = (AstTyped *) entry->key;
        if (node->kind == Ast_Kind_Overloaded_Function) continue;

        if (   node->kind != Ast_Kind_Function
            && node->kind != Ast_Kind_Polymorphic_Proc
            && node->kind != Ast_Kind_Macro) {
            onyx_report_error(node->token->pos, Error_Critical, "Overload option not procedure or macro. Got '%s'",
                onyx_ast_node_kind_string(node->kind));

            bh_imap_free(&all_overloads);
            return Check_Error;
        }

        if (node->kind == Ast_Kind_Function) {
            AstFunction* func = (AstFunction *) node;

            if (func->entity_header && func->entity_header->state <= Entity_State_Check_Types) {
                done = 0;
            }
        }
    }

    bh_imap_free(&all_overloads);

    if (done) return Check_Success;
    else      YIELD(func->token->pos, "Waiting for all options to pass type-checking.");
}

CheckStatus check_struct(AstStructType* s_node) {
    if (s_node->entity_defaults && s_node->entity_defaults->state < Entity_State_Check_Types)
        YIELD(s_node->token->pos, "Waiting for struct member defaults to pass symbol resolution.");

    if (s_node->polymorphic_argument_types) {
        assert(s_node->polymorphic_arguments);

        fori (i, 0, (i64) bh_arr_length(s_node->polymorphic_argument_types)) {
            Type *arg_type = type_build_from_ast(context.ast_alloc, s_node->polymorphic_argument_types[i]);
            if (arg_type == NULL) YIELD(s_node->polymorphic_argument_types[i]->token->pos, "Waiting to build type for polymorph argument.");

            // CLEANUP: This might be wrong...
            if (s_node->polymorphic_arguments[i].value) {
                TYPE_CHECK(&s_node->polymorphic_arguments[i].value, arg_type) {
                    ERROR_(s_node->polymorphic_arguments[i].value->token->pos, "Expected value of type %s, got %s.",
                        type_get_name(arg_type),
                        type_get_name(s_node->polymorphic_arguments[i].value->type));
                }
            }
        }
    }

    if (s_node->constraints.constraints) {
        s_node->constraints.produce_errors = (s_node->flags & Ast_Flag_Header_Check_No_Error) == 0;

        OnyxFilePos pos = s_node->token->pos;
        if (s_node->polymorphic_error_loc.filename) {
            pos = s_node->polymorphic_error_loc;
        }
        CHECK(constraint_context, &s_node->constraints, s_node->scope, pos);
    }

    if (s_node->scope) {
        fori (i, 0, shlen(s_node->scope->symbols)) {
            AstNode* node = s_node->scope->symbols[i].value;
            if (node->kind == Ast_Kind_Function) {
                node->flags |= Ast_Flag_Function_Used;
            }
        }
    }

    bh_arr_each(AstStructMember *, smem, s_node->members) {
        if ((*smem)->type_node != NULL) {
            CHECK(type, &(*smem)->type_node);
        }

        if ((*smem)->type_node == NULL && (*smem)->initial_value != NULL) {
            CHECK(expression, &(*smem)->initial_value);

            fill_in_type((*smem)->initial_value);
            if ((*smem)->initial_value->type == NULL)
                YIELD((*smem)->initial_value->token->pos, "Trying to resolve type for initial value for member.");

            resolve_expression_type((*smem)->initial_value);
            if ((*smem)->type == NULL) (*smem)->type = (*smem)->initial_value->type;

            if ((*smem)->type == NULL) {
                ERROR((*smem)->initial_value->token->pos, "Unable to deduce type of initial value. This is probably a compiler bug.");
            }
        }
    }

    // NOTE: fills in the pending_type.
    s_node->ready_to_build_type = 1;
    type_build_from_ast(context.ast_alloc, (AstType *) s_node);
    if (s_node->pending_type == NULL || !s_node->pending_type_is_valid)
        YIELD(s_node->token->pos, "Waiting for type to be constructed.");

    bh_arr_each(StructMember *, smem, s_node->pending_type->Struct.memarr) {
        if ((*smem)->type->kind == Type_Kind_Compound) {
            ERROR(s_node->token->pos, "Compound types are not allowed as struct member types.");
        }

        if ((*smem)->used) {
            if (!type_struct_member_apply_use(context.ast_alloc, s_node->pending_type, *smem)) {
                YIELD((*smem)->token->pos, "Waiting for use to be applied.");
            }
        }
    }

    s_node->stcache = s_node->pending_type;
    s_node->stcache->Struct.status = SPS_Uses_Done;

    return Check_Success;
}

CheckStatus check_struct_defaults(AstStructType* s_node) {
    if (s_node->entity_type && s_node->entity_type->state < Entity_State_Code_Gen)
        YIELD(s_node->token->pos, "Waiting for struct type to be constructed before checking defaulted members.");
    if (s_node->entity_type && s_node->entity_type->state == Entity_State_Failed)
        return Check_Failed;

    if (s_node->meta_tags) {
        bh_arr_each(AstTyped *, meta, s_node->meta_tags) {
            CHECK(expression, meta);
            resolve_expression_type(*meta);

            if (((*meta)->flags & Ast_Flag_Comptime) == 0) {
                onyx_report_error((*meta)->token->pos, Error_Critical, "#tag expressions are expected to be compile-time known.");
                return Check_Error;
            }
        }
    }

    bh_arr_each(StructMember *, smem, s_node->stcache->Struct.memarr) {
        if ((*smem)->initial_value && *(*smem)->initial_value) {
            CHECK(expression, (*smem)->initial_value);

            TYPE_CHECK((*smem)->initial_value, (*smem)->type) {
                ERROR_((*(*smem)->initial_value)->token->pos,
                        "Mismatched type for initial value, expected '%s', got '%s'.",
                        type_get_name((*smem)->type),
                        type_get_name((*(*smem)->initial_value)->type));
            }

            resolve_expression_type(*(*smem)->initial_value);
        }

        if ((*smem)->meta_tags) {
            bh_arr_each(AstTyped *, meta, (*smem)->meta_tags) {
                CHECK(expression, meta);
                resolve_expression_type(*meta);

                if (((*meta)->flags & Ast_Flag_Comptime) == 0) {
                    onyx_report_error((*meta)->token->pos, Error_Critical, "#tag expressions are expected to be compile-time known.");
                    return Check_Error;
                }
            }
        }
    }

    return Check_Success;
}

CheckStatus check_temp_function_header(AstFunction* func) {
    CheckStatus cs = check_function_header(func);
    if (cs == Check_Error) {
        if (func->flags & Ast_Flag_Header_Check_No_Error) {
            onyx_clear_errors();
        }

        return Check_Failed;
    }

    if (cs != Check_Success) return cs;

    return Check_Complete;
}

CheckStatus check_function_header(AstFunction* func) {
    //if (func->entity_body && func->entity_body->state < Entity_State_Check_Types)
    //    YIELD(func->token->pos, "Waiting for function body to complete symbol resolution to check header.");

    b32 expect_default_param = 0;
    b32 has_had_varargs = 0;

    if (func->constraints.constraints != NULL && func->constraints.constraints_met == 0) {
        func->constraints.produce_errors = (func->flags & Ast_Flag_Header_Check_No_Error) == 0;
        CHECK(constraint_context, &func->constraints, func->scope, func->token->pos);

        // All constraints have been met. Return to symbol resolution to finish
        // looking up all symbols in the function.
        return Check_Return_To_Symres;
    }

    bh_arr_each(AstParam, param, func->params) {
        AstLocal* local = param->local;

        if (expect_default_param && param->default_value == NULL) {
            ERROR(local->token->pos,
                    "All parameters must have default values after the first default valued parameter.");
        }

        if (has_had_varargs && param->vararg_kind != VA_Kind_Not_VA) {
            ERROR(local->token->pos,
                    "Can only have one param that is of variable argument type.");
        }

        if (has_had_varargs && param->vararg_kind != VA_Kind_Not_VA) {
            ERROR(local->token->pos,
                    "Variable arguments must be last in parameter list");
        }

        if (param->vararg_kind == VA_Kind_Untyped) {
            // HACK
            if (builtin_vararg_type_type == NULL)
                builtin_vararg_type_type = type_build_from_ast(context.ast_alloc, builtin_vararg_type);

            local->type = builtin_vararg_type_type;
        }

        if (param->default_value != NULL) {
            if (param->vararg_kind != VA_Kind_Not_VA) {
                ERROR(local->token->pos, "Variadic arguments cannot have default values.");
            }

            CHECK(expression, &param->default_value);

            if (local->type_node == NULL && local->type == NULL) {
                local->type = resolve_expression_type(param->default_value);
            }

            expect_default_param = 1;
        }

        if (local->type_node != NULL) {
            // If the function has the no_error flag, then the type node should have it set too.
            // This allows for polymorphic structures with constraints to fail gracefully.
            local->type_node->flags |= (func->flags & Ast_Flag_Header_Check_No_Error);
            CHECK(type, &local->type_node);
        }

        fill_in_type((AstTyped *) local);
        if (local->type == NULL) {
            YIELD(local->token->pos, "Waiting for parameter type to be known.");
        }

        if (local->type == (Type *) &node_that_signals_failure) {
            return Check_Failed;
        }

        if (local->type->kind == Type_Kind_Compound) {
            ERROR(param->local->token->pos, "Compound types are not allowed as parameter types. Try splitting this into multiple parameters.");
        }

        // NOTE: I decided to make parameter default values not type checked against
        // the actual parameter type. The actual type checking will happen in check_call
        // when the default value is used as an argument and then has to be checked against
        // the parameter type                                  - brendanfh 2021/01/06
        // if (param->default_value != NULL) {
        //     if (!unify_node_and_type(&param->default_value, param->local->type)) {
        //         onyx_report_error(param->local->token->pos,
        //                 "Expected default value of type '%s', was of type '%s'.",
        //                 type_get_name(param->local->type),
        //                 type_get_name(param->default_value->type));
        //         return Check_Error;
        //     }
        // }

        if (param->vararg_kind != VA_Kind_Not_VA) has_had_varargs = 1;

        if (local->type->kind != Type_Kind_Array && type_size_of(local->type) == 0) {
            ERROR(local->token->pos, "Function parameters cannot have zero-width types.");
        }
    }

    if (func->return_type != NULL) CHECK(type, &func->return_type);

    func->type = type_build_function_type(context.ast_alloc, func);
    if (func->type == NULL) YIELD(func->token->pos, "Waiting for function type to be constructed");

    return Check_Success;
}

CheckStatus check_memres_type(AstMemRes* memres) {
    CHECK(type, &memres->type_node);
    fill_in_type((AstTyped *) memres);
    if (memres->type_node && !memres->type) YIELD(memres->token->pos, "Waiting for global type to be constructed.");
    return Check_Success;
}

CheckStatus check_memres(AstMemRes* memres) {
    assert(memres->type_entity);
    if (memres->type_entity->state < Entity_State_Code_Gen) YIELD(memres->token->pos, "Waiting for global to pass type construction.");

    if (memres->initial_value != NULL) {
        if (memres->threadlocal) {
            onyx_report_error(memres->token->pos, Error_Critical, "'#thread_local' variables cannot have an initializer at the moment.");
            return Check_Error;
        }

        CHECK(expression, &memres->initial_value);

        if (memres->type != NULL) {
            Type* memres_type = memres->type;
            TYPE_CHECK(&memres->initial_value, memres_type) {
                ERROR_(memres->token->pos,
                        "Cannot assign value of type '%s' to a '%s'.",
                        node_get_type_name(memres->initial_value),
                        type_get_name(memres_type));
            }

        } else {
            resolve_expression_type(memres->initial_value);
            if (memres->initial_value->type == NULL && memres->initial_value->entity != NULL && memres->initial_value->entity->state <= Entity_State_Check_Types) {
                YIELD(memres->token->pos, "Waiting for global type to be constructed.");
            }
            memres->type = memres->initial_value->type;
        }

        if ((memres->initial_value->flags & Ast_Flag_Comptime) == 0) {
            if (memres->initial_value->entity != NULL && memres->initial_value->entity->state <= Entity_State_Check_Types) {
                YIELD(memres->token->pos, "Waiting for initial value to be checked.");
            }

            ERROR(memres->initial_value->token->pos, "Top level expressions must be compile time known.");
        }
    }

    return Check_Success;
}

CheckStatus check_type(AstType** ptype) {
    if (ptype == NULL || *ptype == NULL) return Check_Success;

    AstType* type = *ptype;
    AstType* original_type = type;
    while (type->kind == Ast_Kind_Type_Alias)
        type = ((AstTypeAlias *) type)->to;

    if (type->flags & Ast_Flag_Has_Been_Checked) return Check_Success;

    switch (type->kind) {
        case Ast_Kind_Poly_Call_Type: {
            AstPolyCallType* pc_node = (AstPolyCallType *) type;

            bh_arr_each(AstNode *, param, pc_node->params) {
                if (!node_is_type(*param)) {
                    CHECK(expression, (AstTyped **) param);
                    resolve_expression_type((AstTyped *) *param);
                    fill_in_type((AstTyped *) *param);
                }
            }

            break;
        }

        case Ast_Kind_Typeof: {
            AstTypeOf *type_of = (AstTypeOf *) type;
            CHECK(expression, (AstTyped **) &type_of->expr);
            resolve_expression_type(type_of->expr);

            if (type_of->expr->type == NULL) {
                YIELD(type_of->token->pos, "Trying to check type for type-of expression.");
            }

            type_of->resolved_type = type_of->expr->type;
            break;
        }

        case Ast_Kind_Pointer_Type: ((AstPointerType *) type)->elem->flags |= type->flags & Ast_Flag_Header_Check_No_Error; CHECK(type, &((AstPointerType *) type)->elem); break;
        case Ast_Kind_Slice_Type:   ((AstSliceType *) type)->elem->flags |= type->flags & Ast_Flag_Header_Check_No_Error; CHECK(type, &((AstSliceType *) type)->elem); break;
        case Ast_Kind_DynArr_Type:  ((AstDynArrType *) type)->elem->flags |= type->flags & Ast_Flag_Header_Check_No_Error; CHECK(type, &((AstDynArrType *) type)->elem); break;
        case Ast_Kind_VarArg_Type:  ((AstVarArgType *) type)->elem->flags |= type->flags & Ast_Flag_Header_Check_No_Error; CHECK(type, &((AstVarArgType *) type)->elem); break;

        case Ast_Kind_Function_Type: {
            AstFunctionType* ftype = (AstFunctionType *) type;

            CHECK(type, &ftype->return_type);

            if (ftype->param_count > 0) {
                fori (i, 0, (i64) ftype->param_count) {
                    CHECK(type, &ftype->params[i]);
                }
            }
            break;
        }

        case Ast_Kind_Type_Compound: {
            AstCompoundType* ctype = (AstCompoundType *) type;

            bh_arr_each(AstType *, type, ctype->types) CHECK(type, type);
            break;
        }

        case Ast_Kind_Array_Type: {
            AstArrayType* atype = (AstArrayType *) type;
            if (atype->count_expr) {
                CHECK(expression, &atype->count_expr);
                resolve_expression_type(atype->count_expr);
            }

            break;
        }

        case Ast_Kind_Field_Access: {
            CHECK(field_access, (AstFieldAccess **) ptype);
            type = *ptype;
            original_type = type;

            if (!node_is_type((AstNode *) type)) {
                ERROR_(original_type->token->pos, "This field access did not resolve to be a type. It resolved to be a '%s'.", onyx_ast_node_kind_string(type->kind));
            }
            break;
        }
    }

    type = original_type;
    type->flags |= Ast_Flag_Comptime;
    while (type->kind == Ast_Kind_Type_Alias) {
        type->flags |= Ast_Flag_Comptime;
        type = ((AstTypeAlias *) type)->to;
    }

    type->flags |= Ast_Flag_Has_Been_Checked;
    return Check_Success;
}

CheckStatus check_static_if(AstIf* static_if) {
    expression_types_must_be_known = 1;
    CheckStatus result = check_expression(&static_if->cond);
    expression_types_must_be_known = 0;
    if (result == Check_Yield_Macro) return Check_Yield_Macro;

    if (result > Check_Errors_Start || !(static_if->cond->flags & Ast_Flag_Comptime)) {
        ERROR(static_if->token->pos, "Expected this condition to be compile time known.");
    }

    if (!type_is_bool(static_if->cond->type)) {
        ERROR(static_if->token->pos, "Expected this condition to be a boolean value.");
    }

    static_if->flags |= Ast_Flag_Static_If_Resolved;

    b32 resolution = static_if_resolution(static_if);

    if (context.options->print_static_if_results)
        bh_printf("Static if statement at %s:%d:%d resulted in %s\n",
            static_if->token->pos.filename,
            static_if->token->pos.line,
            static_if->token->pos.column,
            resolution ? "true" : "false");

    if (resolution) {
        bh_arr_each(Entity *, ent, static_if->true_entities) {
            entity_heap_insert_existing(&context.entities, *ent);
        }

    } else {
        bh_arr_each(Entity *, ent, static_if->false_entities) {
            entity_heap_insert_existing(&context.entities, *ent);
        }
    }

    return Check_Complete;
}

CheckStatus check_process_directive(AstNode* directive) {
    if (directive->kind == Ast_Kind_Directive_Export) {
        AstDirectiveExport *export = (AstDirectiveExport *) directive;
        AstTyped *exported = export->export;
        if (exported->entity && exported->entity->state <= Entity_State_Check_Types)
            YIELD(directive->token->pos, "Waiting for exported type to be known.");

        CHECK(expression, &export->export_name_expr);

        if (export->export_name_expr->kind != Ast_Kind_StrLit) {
            ERROR_(export->token->pos, "Expected export name to be a string literal, got '%s'.", onyx_ast_node_kind_string(export->export_name_expr->kind));
        }

        export->export_name = export->export_name_expr->token;
    }

    if (directive->kind == Ast_Kind_Directive_Init) {
        AstDirectiveInit *init = (AstDirectiveInit *) directive;
        if ((init->flags & Ast_Flag_Has_Been_Checked) == 0) {
            CHECK(expression, &init->init_proc);

            if (init->init_proc->kind != Ast_Kind_Function) {
                ERROR_(init->token->pos, "#init only works for functions, got '%s'", onyx_ast_node_kind_string(init->init_proc->kind));
            }

            assert(init->init_proc->type);
            if (init->init_proc->type->Function.param_count != 0) {
                ERROR(init->token->pos, "#init expects a function that takes 0 arguments.");
            }
        }

        init->flags |= Ast_Flag_Has_Been_Checked;

        if (init->dependencies) {
            i32 i = 0;
            bh_arr_each(AstDirectiveInit *, dependency, init->dependencies) {
                AstTyped *d = (AstTyped *) strip_aliases((AstNode *) *dependency);
                if (d->kind != Ast_Kind_Directive_Init) {
                    ERROR_(init->token->pos, "All dependencies of an #init must be another #init. The %d%s dependency was not.", i + 1, bh_num_suffix(i + 1));
                }

                assert(d->entity);
                if (d->entity->state != Entity_State_Finalized) {
                    YIELD(init->token->pos, "Circular dependency in #init nodes. Here are the nodes involved.");
                }

                i++;
            }
        }

        bh_arr_push(init_procedures, (AstFunction *) init->init_proc);
        return Check_Complete;
    }

    if (directive->kind == Ast_Kind_Directive_Library) {
        AstDirectiveLibrary *library = (AstDirectiveLibrary *) directive;

        if (library->library_symbol->kind != Ast_Kind_StrLit) {
            ERROR_(library->token->pos, "#library directive expected compile-time known string for library name. Got '%s'.",
                onyx_ast_node_kind_string(library->library_symbol->kind));
        }

        AstStrLit *symbol = (AstStrLit *) library->library_symbol;
        char* temp_name     = bh_alloc_array(global_scratch_allocator, char, symbol->token->length);
        i32   temp_name_len = string_process_escape_seqs(temp_name, symbol->token->text, symbol->token->length);
        library->library_name = bh_strdup(global_heap_allocator, temp_name);
        return Check_Success;
    }

    return Check_Success;
}

CheckStatus check_macro(AstMacro* macro) {
    if (macro->body->kind == Ast_Kind_Function) {
        CHECK(function_header, (AstFunction *) macro->body);
    }

    return Check_Success;
}

CheckStatus check_constraint(AstConstraint *constraint) {
    switch (constraint->phase) {
        case Constraint_Phase_Cloning_Expressions: {
            if (constraint->interface->kind == Ast_Kind_Symbol) {
                return Check_Return_To_Symres;
            }

            if (constraint->interface->kind != Ast_Kind_Interface) {
                // CLEANUP: This error message might not look totally right in some cases.
                ERROR_(constraint->token->pos, "'%b' is not an interface. It is a '%s'.",
                    constraint->token->text, constraint->token->length,
                    onyx_ast_node_kind_string(constraint->interface->kind));
            }

            bh_arr_new(global_heap_allocator, constraint->exprs, bh_arr_length(constraint->interface->exprs));
            bh_arr_each(InterfaceConstraint, ic, constraint->interface->exprs) {
                InterfaceConstraint new_ic = {0};
                new_ic.expr = (AstTyped *) ast_clone(context.ast_alloc, (AstNode *) ic->expr);
                new_ic.expected_type_expr = (AstType *) ast_clone(context.ast_alloc, (AstNode *) ic->expected_type_expr);
                new_ic.invert_condition = ic->invert_condition;
                bh_arr_push(constraint->exprs, new_ic);
            }

            assert(constraint->interface->entity && constraint->interface->entity->scope);

            constraint->scope = scope_create(context.ast_alloc, constraint->interface->entity->scope, constraint->token->pos);

            fori (i, 0, bh_arr_length(constraint->interface->params)) {
                InterfaceParam *ip = &constraint->interface->params[i];

                AstTyped *sentinel = onyx_ast_node_new(context.ast_alloc, sizeof(AstTyped), Ast_Kind_Constraint_Sentinel);
                sentinel->token = ip->value_token;
                sentinel->type_node = constraint->type_args[i];

                AstAlias *type_alias = onyx_ast_node_new(context.ast_alloc, sizeof(AstAlias), Ast_Kind_Alias);
                type_alias->token = ip->type_token;
                type_alias->alias = (AstTyped *) constraint->type_args[i];

                symbol_introduce(constraint->scope, ip->value_token, (AstNode *) sentinel);
                symbol_introduce(constraint->scope, ip->type_token, (AstNode *) type_alias);
            }

            assert(constraint->entity);
            constraint->entity->scope = constraint->scope;

            constraint->phase = Constraint_Phase_Checking_Expressions;
            return Check_Return_To_Symres;
        }

        case Constraint_Phase_Checking_Expressions: {
            fori (i, constraint->expr_idx, bh_arr_length(constraint->exprs)) {
                InterfaceConstraint* ic = &constraint->exprs[i];

                CheckStatus cs = check_expression(&ic->expr);
                if (cs == Check_Return_To_Symres || cs == Check_Yield_Macro) {
                    return cs;
                }

                if (cs == Check_Error && !ic->invert_condition) {
                    goto constraint_error;
                }

                if (cs == Check_Success && ic->invert_condition) {
                    goto constraint_error;
                }

                if (ic->expected_type_expr) {
                    cs = check_type(&ic->expected_type_expr);
                    if (cs == Check_Return_To_Symres || cs == Check_Yield_Macro) {
                        return cs;
                    }

                    ic->expected_type = type_build_from_ast(context.ast_alloc, ic->expected_type_expr);
                    if (ic->expected_type == NULL) {
                        YIELD(ic->expected_type_expr->token->pos, "Waiting on expected type expression to be resolved.");
                    }

                    TYPE_CHECK(&ic->expr, ic->expected_type) {
                        if (!ic->invert_condition)
                            goto constraint_error;
                    }
                }

                constraint->expr_idx++;
                continue;

              constraint_error:
                // HACK HACK HACK
                onyx_clear_errors();
                *constraint->report_status = Constraint_Check_Status_Failed;
                return Check_Failed;
            }

            // HACK HACK HACK
            onyx_clear_errors();
            *constraint->report_status = Constraint_Check_Status_Success;
            return Check_Complete;
        }
    }

    return Check_Success;
}

CheckStatus check_constraint_context(ConstraintContext *cc, Scope *scope, OnyxFilePos pos) {
    if (cc->constraint_checks) {
        if (cc->constraints_met == 1) return Check_Success;

        fori (i, 0, bh_arr_length(cc->constraints)) {
            if (cc->constraint_checks[i] == Constraint_Check_Status_Failed) {
                if (cc->produce_errors) {
                    AstConstraint *constraint = cc->constraints[i];
                    char constraint_map[512] = {0};
                    fori (i, 0, bh_arr_length(constraint->type_args)) {
                        if (i != 0) strncat(constraint_map, ", ", 511);

                        OnyxToken* symbol = constraint->interface->params[i].value_token;
                        token_toggle_end(symbol);
                        strncat(constraint_map, symbol->text, 511);
                        token_toggle_end(symbol);

                        strncat(constraint_map, " is of type '", 511);
                        strncat(constraint_map, type_get_name(type_build_from_ast(context.ast_alloc, constraint->type_args[i])), 511);
                        strncat(constraint_map, "'", 511);
                    }

                    onyx_report_error(constraint->exprs[constraint->expr_idx].expr->token->pos, Error_Critical, "Failed to satisfy constraint where %s.", constraint_map);
                    onyx_report_error(constraint->token->pos, Error_Critical, "Here is where the interface was used.");
                    onyx_report_error(pos, Error_Critical, "Here is the code that caused this constraint to be checked.");

                    return Check_Error;

                } else {
                    // If no error are suppose to be produced, we still need to signal that
                    // the node reached a completed state.
                    return Check_Failed;
                }
            }

            if (cc->constraint_checks[i] == Constraint_Check_Status_Queued) {
                YIELD(pos, "Waiting for constraints to be checked.");
            }
        }

        cc->constraints_met = 1;
        return Check_Success;

    } else {
        u32 count = bh_arr_length(cc->constraints);
        ConstraintCheckStatus *ccs = bh_alloc_array(context.ast_alloc, ConstraintCheckStatus, count);

        cc->constraint_checks = ccs;

        fori (i, 0, count) {
            ccs[i] = Constraint_Check_Status_Queued;
            cc->constraints[i]->report_status = &ccs[i];
            cc->constraints[i]->phase = Constraint_Phase_Cloning_Expressions;

            add_entities_for_node(NULL, (AstNode *) cc->constraints[i], scope, NULL);
        }

        return Check_Yield_Macro;
    }
}

CheckStatus check_polyquery(AstPolyQuery *query) {
    if (query->function_header->scope == NULL)
        query->function_header->scope = scope_create(context.ast_alloc, query->proc->parent_scope_of_poly_proc, query->token->pos);

    CheckStatus header_check = check_temp_function_header(query->function_header);
    if (header_check == Check_Return_To_Symres) return Check_Return_To_Symres;

    b32 solved_something = 0;
    i32 solved_count = 0;
    char *err_msg = NULL;
    bh_arr_each(AstPolyParam, param, query->proc->poly_params) {
        AstPolySolution sln;
        bh_arr_each(AstPolySolution, solved_sln, query->slns) {
            if (token_equals(param->poly_sym->token, solved_sln->poly_sym->token)) {
                goto poly_query_done;
            }
        }

        // CLEANUP: I think this can go away because it is already done in polymorph.c
        bh_arr_each(AstPolySolution, known_sln, query->proc->known_slns) {
            if (token_equals(param->poly_sym->token, known_sln->poly_sym->token)) {
                sln = *known_sln;
                goto poly_var_solved;
            }
        }

        TypeMatch result = find_polymorphic_sln(&sln, param, query->function_header, query->pp_lookup, query->given, &err_msg);

        switch (result) {
            case TYPE_MATCH_SUCCESS:
                goto poly_var_solved;

            case TYPE_MATCH_SPECIAL:
                return Check_Yield_Macro;

            case TYPE_MATCH_YIELD:
            case TYPE_MATCH_FAILED: {
                if (query->successful_symres || solved_something) continue;

                if (query->error_on_fail || context.cycle_detected) {
                    onyx_report_error(query->token->pos, Error_Critical, "Error solving for polymorphic variable '%b'.", param->poly_sym->token->text, param->poly_sym->token->length);
                    if (err_msg != NULL)  onyx_report_error(query->token->pos, Error_Critical, "%s", err_msg);
                    if (query->error_loc) onyx_report_error(query->error_loc->pos, Error_Critical, "Here is where the call is located."); // :ErrorMessage
                }

                return Check_Failed;
            }
        }

poly_var_solved:
        solved_something = 1;
        bh_arr_push(query->slns, sln);
        insert_poly_sln_into_scope(query->function_header->scope, &sln);

poly_query_done:
        solved_count += 1;
    }

    if (solved_count != bh_arr_length(query->proc->poly_params)) {
        if (solved_something || query->successful_symres) {
            return Check_Return_To_Symres;
        } else {
            return Check_Failed;
        }
    }

    return Check_Complete;
}

void check_entity(Entity* ent) {
    CheckStatus cs = Check_Success;

    switch (ent->type) {
        case Entity_Type_Foreign_Function_Header:
        case Entity_Type_Function_Header:          cs = check_function_header(ent->function); break;
        case Entity_Type_Temp_Function_Header:     cs = check_temp_function_header(ent->function); break;
        case Entity_Type_Function:                 cs = check_function(ent->function); break;
        case Entity_Type_Overloaded_Function:      cs = check_overloaded_function(ent->overloaded_function); break;
        case Entity_Type_Global:                   cs = check_global(ent->global); break;
        case Entity_Type_Struct_Member_Default:    cs = check_struct_defaults((AstStructType *) ent->type_alias); break;
        case Entity_Type_Memory_Reservation_Type:  cs = check_memres_type(ent->mem_res); break;
        case Entity_Type_Memory_Reservation:       cs = check_memres(ent->mem_res); break;
        case Entity_Type_Static_If:                cs = check_static_if(ent->static_if); break;
        case Entity_Type_Macro:                    cs = check_macro(ent->macro); break;
        case Entity_Type_Constraint_Check:         cs = check_constraint(ent->constraint); break;
        case Entity_Type_Polymorph_Query:          cs = check_polyquery(ent->poly_query); break;
        case Entity_Type_Enum_Value:               cs = check_expression(&ent->enum_value->value); break;
        case Entity_Type_Process_Directive:        cs = check_process_directive((AstNode *) ent->expr); break;

        case Entity_Type_Expression:
            cs = check_expression(&ent->expr);
            resolve_expression_type(ent->expr);
            break;

        case Entity_Type_Type_Alias:
            if (ent->type_alias->kind == Ast_Kind_Struct_Type)
                cs = check_struct((AstStructType *) ent->type_alias);
            else
                cs = check_type(&ent->type_alias);
            break;

        case Entity_Type_File_Contents:
            if (context.options->no_file_contents) {
                onyx_report_error(ent->expr->token->pos, Error_Critical, "#file_contents is disabled for this compilation.");
            }
            break;

        default: break;
    }

    switch (cs) {
        case Check_Yield_Macro:      ent->macro_attempts++; break;
        case Check_Success:          ent->state = Entity_State_Code_Gen;        goto clear_attempts;
        case Check_Complete:         ent->state = Entity_State_Finalized;       goto clear_attempts;
        case Check_Return_To_Symres: ent->state = Entity_State_Resolve_Symbols; goto clear_attempts;
        case Check_Failed:           ent->state = Entity_State_Failed;          goto clear_attempts;

    clear_attempts:
        ent->macro_attempts = 0;
        ent->micro_attempts = 0;
    }
}
