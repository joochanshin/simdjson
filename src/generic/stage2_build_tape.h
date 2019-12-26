// This file contains the common code every implementation uses for stage2
// It is intended to be included multiple times and compiled multiple times
// We assume the file in which it is include already includes
// "simdjson/stage2_build_tape.h" (this simplifies amalgation)

const size_t BLOCK_SIZE = 64;

really_inline void advance(const uint8_t*& block, int& offset, uint64_t*& structurals) {
  // If there are no more structurals in this block, advance to the next
  bool is_last_structural_in_block = *structurals == 0;
  structurals += is_last_structural_in_block;
  block += is_last_structural_in_block*BLOCK_SIZE;

  // Steal the trailing bit, taking the next structural offset
  offset = trailing_zeroes(*structurals);
  *structurals = clear_lowest_bit(*structurals);
}

// This macro reads the next structural character, updating block, offset
// and structurals. Returns the current character (which can be reached at
// block[offset]).
#define ADVANCE() (advance(block, offset, structurals), block[offset])
#define PARSE_STRING()                                \
  {                                                   \
    int new_offset = parse_string(block, offset, pj); \
    if (unlikely(new_offset == 0)) {                  \
      goto fail;                                      \
    }                                                 \
    /* If it completely skipped any blocks, advance block */ \
    int blocks_skipped = new_offset / BLOCK_SIZE - 1; \
    blocks_skipped = (blocks_skipped < 0) ? 0 : blocks_skipped; /* Saturating sub via CMOV */ \
    block += BLOCK_SIZE*blocks_skipped;               \
  }
#define ELSE_CHAR(LABEL) \
  case ' ':     \
  case '\n':    \
  case '\r':    \
  case '\t':    \
    goto LABEL; \
  default:      \
    goto fail;

#ifdef SIMDJSON_USE_COMPUTED_GOTO
#define SET_GOTO_ARRAY_CONTINUE() pj.ret_address[depth] = &&array_continue;
#define SET_GOTO_OBJECT_CONTINUE() pj.ret_address[depth] = &&object_continue;
#define SET_GOTO_START_CONTINUE() pj.ret_address[depth] = &&start_continue;
#define GOTO_CONTINUE() goto *pj.ret_address[depth];
#else
#define SET_GOTO_ARRAY_CONTINUE() pj.ret_address[depth] = 'a';
#define SET_GOTO_OBJECT_CONTINUE() pj.ret_address[depth] = 'o';
#define SET_GOTO_START_CONTINUE() pj.ret_address[depth] = 's';
#define GOTO_CONTINUE()                                                        \
  {                                                                            \
    if (pj.ret_address[depth] == 'a') {                                        \
      goto array_continue;                                                     \
    } else if (pj.ret_address[depth] == 'o') {                                 \
      goto object_continue;                                                    \
    } else {                                                                   \
      goto start_continue;                                                     \
    }                                                                          \
  }
#endif

/************
 * The JSON is parsed to a tape, see the accompanying tape.md file
 * for documentation.
 ***********/
WARN_UNUSED int
unified_machine(const uint8_t *buf, size_t len, ParsedJson &pj) {
  pj.init();          // sets is_valid to false
  if (pj.byte_capacity < len) {
    pj.error_code = CAPACITY;
    return pj.error_code;
  }

  const uint8_t *block = buf;
  uint32_t depth = 0;
  uint64_t* structurals = pj.structural_blocks;
  int offset = trailing_zeroes(*structurals);
  *structurals = clear_lowest_bit(*structurals);

  /*//////////////////////////// START STATE /////////////////////////////
   */
  SET_GOTO_START_CONTINUE();
  pj.containing_scope_offset[depth] = pj.get_current_loc();
  pj.write_tape(0, 'r'); /* r for root, 0 is going to get overwritten */
  /* the root is used, if nothing else, to capture the size of the tape */
  depth++; /* everything starts at depth = 1, depth = 0 is just for the
              root, the root may contain an object, an array or something
              else. */
  if (depth >= pj.depth_capacity) {
    goto fail;
  }

start:
  switch (block[offset]) {
  case '{':
    pj.containing_scope_offset[depth] = pj.get_current_loc();
    SET_GOTO_START_CONTINUE();
    depth++;
    if (depth >= pj.depth_capacity) {
      goto fail;
    }
    pj.write_tape(0, block[offset]); /* strangely, moving this to object_begin slows things down */
    goto object_begin;
  case '[':
    pj.containing_scope_offset[depth] = pj.get_current_loc();
    SET_GOTO_START_CONTINUE();
    depth++;
    if (depth >= pj.depth_capacity) {
      goto fail;
    }
    pj.write_tape(0, block[offset]);
    goto array_begin;
    /* #define SIMDJSON_ALLOWANYTHINGINROOT
     * A JSON text is a serialized value.  Note that certain previous
     * specifications of JSON constrained a JSON text to be an object or an
     * array.  Implementations that generate only objects or arrays where a
     * JSON text is called for will be interoperable in the sense that all
     * implementations will accept these as conforming JSON texts.
     * https://tools.ietf.org/html/rfc8259
     * #ifdef SIMDJSON_ALLOWANYTHINGINROOT */
  case '"': {
    PARSE_STRING();
    break;
  }
  case 't': {
    /* we need to make a copy to make sure that the string is space
     * terminated.
     * this only applies to the JSON document made solely of the true value.
     * this will almost never be called in practice */
    char *copy = static_cast<char *>(malloc(len + SIMDJSON_PADDING));
    if (copy == nullptr) {
      goto fail;
    }
    memcpy(copy, buf, len);
    memset(copy + len, ' ', sizeof(uint64_t));
    if (!is_valid_true_atom(reinterpret_cast<const uint8_t *>(copy) + (block-buf+offset))) {
      free(copy);
      goto fail;
    }
    free(copy);
    pj.write_tape(0, block[offset]);
    break;
  }
  case 'f': {
    /* we need to make a copy to make sure that the string is space
     * terminated.
     * this only applies to the JSON document made solely of the false
     * value.
     * this will almost never be called in practice */
    
    char *copy = static_cast<char *>(malloc(len + SIMDJSON_PADDING));
    if (copy == nullptr) {
      goto fail;
    }
    memcpy(copy, buf, len);
    memset(copy + len, ' ', sizeof(uint64_t));
    if (!is_valid_false_atom(reinterpret_cast<const uint8_t *>(copy) + (block-buf+offset))) {
      free(copy);
      goto fail;
    }
    free(copy);
    pj.write_tape(0, block[offset]);
    break;
  }
  case 'n': {
    /* we need to make a copy to make sure that the string is space
     * terminated.
     * this only applies to the JSON document made solely of the null value.
     * this will almost never be called in practice */
    char *copy = static_cast<char *>(malloc(len + SIMDJSON_PADDING));
    if (copy == nullptr) {
      goto fail;
    }
    memcpy(copy, buf, len);
    memset(copy + len, ' ', sizeof(uint64_t));
    if (!is_valid_null_atom(reinterpret_cast<const uint8_t *>(copy) + (block-buf+offset))) {
      free(copy);
      goto fail;
    }
    free(copy);
    pj.write_tape(0, block[offset]);
    break;
  }
  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
  case '8':
  case '9': {
    /* we need to make a copy to make sure that the string is space
     * terminated.
     * this is done only for JSON documents made of a sole number
     * this will almost never be called in practice. We terminate with a
     * space
     * because we do not want to allow NULLs in the middle of a number
     * (whereas a
     * space in the middle of a number would be identified in stage 1). */
    char *copy = static_cast<char *>(malloc(len + SIMDJSON_PADDING));
    if (copy == nullptr) {
      goto fail;
    }
    memcpy(copy, buf, len);
    memset(copy + len, ' ', SIMDJSON_PADDING);
    if (!parse_number(reinterpret_cast<const uint8_t *>(copy), pj, (block-buf+offset), false)) {
      free(copy);
      goto fail;
    }
    free(copy);
    break;
  }
  case '-': {
    /* we need to make a copy to make sure that the string is NULL
     * terminated.
     * this is done only for JSON documents made of a sole number
     * this will almost never be called in practice */
    char *copy = static_cast<char *>(malloc(len + SIMDJSON_PADDING));
    if (copy == nullptr) {
      goto fail;
    }
    memcpy(copy, buf, len);
    memset(copy + len, ' ', SIMDJSON_PADDING);
    if (!parse_number(reinterpret_cast<const uint8_t *>(copy), pj, (block-buf+offset), true)) {
      free(copy);
      goto fail;
    }
    free(copy);
    break;
  }
  ELSE_CHAR(start);
  }
start_continue:
  // Validate that this is the last structural
  if (*structurals == 0 && *(structurals+1) == 0) {
    goto succeed;
  } else {
    goto fail;
  }
  /*//////////////////////////// OBJECT STATES ///////////////////////////*/

object_begin:
  switch (ADVANCE()) {
  case '"': {
    PARSE_STRING();
    goto object_key_state;
  }
  case '}':
    goto scope_end; /* could also go to object_continue */
  default:
    goto fail;
  }

object_key_state:
  if (ADVANCE() != ':') {
    switch (block[offset]) {
      ELSE_CHAR(object_key_state)
    }
  }
object_value_state:
  switch (ADVANCE()) {
  case '"': {
    PARSE_STRING();
    break;
  }
  case 't':
    if (!is_valid_true_atom(&block[offset])) {
      goto fail;
    }
    pj.write_tape(0, block[offset]);
    break;
  case 'f':
    if (!is_valid_false_atom(&block[offset])) {
      goto fail;
    }
    pj.write_tape(0, block[offset]);
    break;
  case 'n':
    if (!is_valid_null_atom(&block[offset])) {
      goto fail;
    }
    pj.write_tape(0, block[offset]);
    break;
  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
  case '8':
  case '9': {
    if (!parse_number(block, pj, offset, false)) {
      goto fail;
    }
    break;
  }
  case '-': {
    if (!parse_number(block, pj, offset, true)) {
      goto fail;
    }
    break;
  }
  case '{': {
    pj.containing_scope_offset[depth] = pj.get_current_loc();
    pj.write_tape(0, block[offset]); /* here the compilers knows what block[offset] is so this gets optimized */
    /* we have not yet encountered } so we need to come back for it */
    SET_GOTO_OBJECT_CONTINUE()
    /* we found an object inside an object, so we need to increment the
     * depth                                                             */
    depth++;
    if (depth >= pj.depth_capacity) {
      goto fail;
    }

    goto object_begin;
  }
  case '[': {
    pj.containing_scope_offset[depth] = pj.get_current_loc();
    pj.write_tape(0, block[offset]); /* here the compilers knows what block[offset] is so this gets optimized */
    /* we have not yet encountered } so we need to come back for it */
    SET_GOTO_OBJECT_CONTINUE()
    /* we found an array inside an object, so we need to increment the depth
     */
    depth++;
    if (depth >= pj.depth_capacity) {
      goto fail;
    }
    goto array_begin;
  }
  ELSE_CHAR(object_value_state)
  }

object_continue:
  switch (ADVANCE()) {
  case ',':
    if (ADVANCE() != '"') {
      goto fail;
    } else {
      PARSE_STRING();
      goto object_key_state;
    }
  case '}':
    goto scope_end;
  ELSE_CHAR(object_continue)
  }

  /*//////////////////////////// COMMON STATE ///////////////////////////*/

scope_end:
  /* write our tape location to the header scope */
  depth--;
  pj.write_tape(pj.containing_scope_offset[depth], block[offset]);
  pj.annotate_previous_loc(pj.containing_scope_offset[depth],
                           pj.get_current_loc());
  /* goto saved_state */
  GOTO_CONTINUE()

  /*//////////////////////////// ARRAY STATES ///////////////////////////*/
array_begin:
  switch (ADVANCE()) {
    case ']':
      goto scope_end;
    case ' ':
    case '\t':
    case '\r':
    case '\n':
      goto array_begin;
  }

main_array_switch:
  /* we call update char on all paths in, so we can peek at current on the
   * on paths that can accept a close square brace (post-, and at start) */
  switch (block[offset]) {
  case '"': {
    PARSE_STRING();
    break;
  }
  case 't':
    if (!is_valid_true_atom(&block[offset])) {
      goto fail;
    }
    pj.write_tape(0, block[offset]);
    break;
  case 'f':
    if (!is_valid_false_atom(&block[offset])) {
      goto fail;
    }
    pj.write_tape(0, block[offset]);
    break;
  case 'n':
    if (!is_valid_null_atom(&block[offset])) {
      goto fail;
    }
    pj.write_tape(0, block[offset]);
    break; /* goto array_continue; */

  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
  case '8':
  case '9': {
    if (!parse_number(block, pj, offset, false)) {
      goto fail;
    }
    break; /* goto array_continue; */
  }
  case '-': {
    if (!parse_number(block, pj, offset, true)) {
      goto fail;
    }
    break; /* goto array_continue; */
  }
  case '{': {
    /* we have not yet encountered ] so we need to come back for it */
    pj.containing_scope_offset[depth] = pj.get_current_loc();
    pj.write_tape(0, block[offset]); /* here the compilers knows what block[offset] is so this gets optimized */
    SET_GOTO_ARRAY_CONTINUE()
    /* we found an object inside an array, so we need to increment the depth
     */
    depth++;
    if (depth >= pj.depth_capacity) {
      goto fail;
    }

    goto object_begin;
  }
  case '[': {
    /* we have not yet encountered ] so we need to come back for it */
    pj.containing_scope_offset[depth] = pj.get_current_loc();
    pj.write_tape(0, block[offset]); /* here the compilers knows what block[offset] is so this gets optimized */
    SET_GOTO_ARRAY_CONTINUE()
    /* we found an array inside an array, so we need to increment the depth
     */
    depth++;
    if (depth >= pj.depth_capacity) {
      goto fail;
    }
    goto array_begin;
  }
  case ' ':
  case '\r':
  case '\t':
  case '\n':
    ADVANCE();
    goto main_array_switch;
  default:
    goto fail;
  }

array_continue:
  switch (ADVANCE()) {
  case ',':
    ADVANCE();
    goto main_array_switch;
  case ']':
    goto scope_end;
  ELSE_CHAR(array_continue)
  }

  /*//////////////////////////// FINAL STATES ///////////////////////////*/

succeed:
  depth--;
  if (depth != 0) {
    fprintf(stderr, "internal bug\n");
    abort();
  }
  if (pj.containing_scope_offset[depth] != 0) {
    fprintf(stderr, "internal bug\n");
    abort();
  }
  pj.annotate_previous_loc(pj.containing_scope_offset[depth],
                           pj.get_current_loc());
  pj.write_tape(pj.containing_scope_offset[depth], 'r'); /* r is root */

  pj.valid = true;
  pj.error_code = simdjson::SUCCESS;
  return pj.error_code;
fail:
  /* we do not need the next line because this is done by pj.init(),
   * pessimistically.
   * pj.is_valid  = false;
   * At this point in the code, we have all the time in the world.
   * Note that we know exactly where we are in the document so we could,
   * without any overhead on the processing code, report a specific
   * location.
   * We could even trigger special code paths to assess what happened
   * carefully,
   * all without any added cost. */
  if (depth >= pj.depth_capacity) {
    pj.error_code = simdjson::DEPTH_ERROR;
    return pj.error_code;
  }
  switch (block[offset]) {
  case '"':
    pj.error_code = simdjson::STRING_ERROR;
    return pj.error_code;
  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
  case '8':
  case '9':
  case '-':
    pj.error_code = simdjson::NUMBER_ERROR;
    return pj.error_code;
  case 't':
    pj.error_code = simdjson::T_ATOM_ERROR;
    return pj.error_code;
  case 'n':
    pj.error_code = simdjson::N_ATOM_ERROR;
    return pj.error_code;
  case 'f':
    pj.error_code = simdjson::F_ATOM_ERROR;
    return pj.error_code;
  default:
    break;
  }
  pj.error_code = simdjson::TAPE_ERROR;
  return pj.error_code;
}
