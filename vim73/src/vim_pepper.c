/*
 * Copyright (c) 2013 The Native Client Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <assert.h>
#include <fcntl.h>
#include <libtar.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "ppapi/c/ppb_messaging.h"
#include "ppapi/c/ppb_var.h"
#include "ppapi/c/ppb_var_dictionary.h"
#include "ppapi_simple/ps.h"
#include "ppapi_simple/ps_event.h"
#include "ppapi_simple/ps_interface.h"
#include "nacl_io/nacl_io.h"

#include "vim.h"
#include "vim_pepper.h"
#include "collab_structs.h"

/*
 * Defined in main.c, vim's own main method.
 */
extern int nacl_vim_main(int argc, char *argv[]);

/*
 * Interface variables for manipulating PP vars/dictionaries.
 */
static const PPB_Var *ppb_var;
static const PPB_VarDictionary *ppb_dict;
static const PPB_Messaging *ppb_msg;
static PP_Instance pp_ins;

/*
 * Strings for each collabtype_T used in parsing messages from JS.
 */
static struct PP_Var type_append_line;
static struct PP_Var type_insert_text;
static struct PP_Var type_remove_line;
static struct PP_Var type_delete_text;
static struct PP_Var type_replace_line;
static struct PP_Var type_key;
static struct PP_Var line_key;
static struct PP_Var text_key;
static struct PP_Var index_key;
static struct PP_Var length_key;

/*
 * Sets up a nacl_io filesystem for vim's runtime files, such as the vimrc and
 * help files. The 'tarfile' contains the http filesystem.
 */
static int setup_unix_environment(const char* tarfile) {
  // Extra tar achive from http filesystem.
  int ret;
  TAR* tar;
  char filename[PATH_MAX];
  strcpy(filename, "/mnt/http/");
  strcat(filename, tarfile);
  ret = tar_open(&tar, filename, NULL, O_RDONLY, 0, 0);
  if (ret) {
    printf("error opening %s\n", filename);
    return 1;
  }

  ret = tar_extract_all(tar, "/");
  if (ret) {
    printf("error extracting %s\n", filename);
    return 1;
  }

  ret = tar_close(tar);
  assert(ret == 0);
  return 0;
}

/*
 * A macro to convert a null-terminated C string to a PP_Var.
 */
#define UTF8_TO_VAR(str) ppb_var->VarFromUtf8(str, strlen(str))

/*
 * Returns a null-terminated C string converted from a string PP_Var.
 */
static char* var_to_cstr(struct PP_Var str_var) {
  const char *vstr;
  uint32_t var_len;
  vstr = ppb_var->VarToUtf8(str_var, &var_len);
  char *cstr = malloc(var_len + 1); // +1 for null char
  strncpy(cstr, vstr, var_len);
  cstr[var_len] = '\0'; // Null terminate the string
  return cstr;
}

/*
 * Just like strcmp, but functions on two PP_Vars.
 * Caller's responsibility to ensure v1 and v2 are strings.
 */
static int pp_strcmp(const struct PP_Var v1, const struct PP_Var v2) {
  uint32_t v1len, v2len;
  const char *v1str, *v2str;
  v1str = ppb_var->VarToUtf8(v1, &v1len);
  v2str = ppb_var->VarToUtf8(v2, &v2len);
  size_t n = (v1len < v2len)? v1len : v2len;
  int cmp = strncmp(v1str, v2str, n);
  if (!cmp)
    cmp = v1len - v2len;
  return cmp;
}

/*
 * Waits for and handles all JS -> NaCL messages.
 * Unused parameter so this function can be used with pthreads. It seems that
 * pnacl-clang doesn't like unnamed parameters.
 */
static void* js_msgloop(void *unused) {
  // Filter to all JS messages.
  PSEventSetFilter(PSE_INSTANCE_HANDLEMESSAGE);
  PSEvent* event;
  while (1) {
    // Wait for the next event.
    event = PSEventWaitAcquire();
    struct PP_Var dict = event->as_var;
    // Ignore anything that isn't a dictionary representing a collabedit.
    if (dict.type != PP_VARTYPE_DICTIONARY ||
        !ppb_dict->HasKey(dict, type_key)) {
      // TODO(zpotter): PSEventRelease here? Or does another handler get the message next?
      js_printf("info: msgloop skipping non collabedit dict");
      continue;
    }

    //  Create a collabedit_T to later enqueue and apply.
    collabedit_T *edit = (collabedit_T *) malloc(sizeof(collabedit_T));
    edit->file_buf = curbuf; // TODO(zpotter): Set actual buffer

    // Parse the specific type of collabedit.
    struct PP_Var var_type = ppb_dict->Get(dict, type_key);
    if (pp_strcmp(var_type, type_append_line) == 0) {
      edit->type = COLLAB_APPEND_LINE;
      edit->append_line.line = ppb_dict->Get(dict, line_key).value.as_int;
      edit->append_line.text = (char_u *)var_to_cstr(ppb_dict->Get(dict, text_key));

    } else if (pp_strcmp(var_type, type_insert_text) == 0) {
      edit->type = COLLAB_INSERT_TEXT;
      edit->insert_text.line = ppb_dict->Get(dict, line_key).value.as_int;
      edit->insert_text.index = ppb_dict->Get(dict, index_key).value.as_int;
      edit->insert_text.text = (char_u *)var_to_cstr(ppb_dict->Get(dict, text_key));

    } else if (pp_strcmp(var_type, type_remove_line) == 0) {
      edit->type = COLLAB_REMOVE_LINE;
      edit->remove_line.line = ppb_dict->Get(dict, line_key).value.as_int;

    } else if (pp_strcmp(var_type, type_delete_text) == 0) {
      edit->type = COLLAB_DELETE_TEXT;
      edit->delete_text.line = ppb_dict->Get(dict, line_key).value.as_int;
      edit->delete_text.index = ppb_dict->Get(dict, index_key).value.as_int;
      edit->delete_text.length = ppb_dict->Get(dict, length_key).value.as_int;

    } else if (pp_strcmp(var_type, type_replace_line) == 0) {
      edit->type = COLLAB_REPLACE_LINE;
      edit->replace_line.line = ppb_dict->Get(dict, line_key).value.as_int;
      edit->replace_line.text = (char_u *)var_to_cstr(ppb_dict->Get(dict, text_key));

    } else {
      // Unknown collabtype_T
      js_printf("info: msgloop unknown collabedit type");
      free(edit);
      edit = NULL;
    }
    
    // Enqueue the edit for processing from the main thread.
    if (edit != NULL)
      collab_enqueue(&collab_queue, edit);
    PSEventRelease(event);
  } 
  // Never reached.
  return NULL;
}

/*
 * Function prototype declared in proto/collaborate.pro, extern decleration
 * in collaborate.c.
 *
 * This implementation of the function sends collabedits to the Drive Realtime
 * model via Pepper messaging.
 */
void collab_remoteapply(collabedit_T *edit) {
  // Turn edit into a PP_Var.
  struct PP_Var dict = ppb_dict->Create();
  // TODO(zpotter): set file_buf
  switch (edit->type) {
    case COLLAB_APPEND_LINE:
      ppb_dict->Set(dict, type_key, type_append_line);
      ppb_dict->Set(dict, line_key, PP_MakeInt32(edit->append_line.line));
      ppb_dict->Set(dict, text_key, UTF8_TO_VAR((char *)edit->append_line.text));
      break;
    case COLLAB_INSERT_TEXT:
      ppb_dict->Set(dict, type_key, type_insert_text);
      ppb_dict->Set(dict, line_key, PP_MakeInt32(edit->insert_text.line));
      ppb_dict->Set(dict, index_key, PP_MakeInt32(edit->insert_text.index));
      ppb_dict->Set(dict, text_key, UTF8_TO_VAR((char *)edit->insert_text.text));
      break;
    case COLLAB_REMOVE_LINE:
      ppb_dict->Set(dict, type_key, type_remove_line);
      ppb_dict->Set(dict, line_key, PP_MakeInt32(edit->remove_line.line));
      break;
    case COLLAB_DELETE_TEXT:
      ppb_dict->Set(dict, type_key, type_delete_text);
      ppb_dict->Set(dict, line_key, PP_MakeInt32(edit->delete_text.line));
      ppb_dict->Set(dict, index_key, PP_MakeInt32(edit->delete_text.index));
      ppb_dict->Set(dict, length_key, PP_MakeInt32(edit->delete_text.length));
      break;
    case COLLAB_REPLACE_LINE:
      ppb_dict->Set(dict, type_key, type_replace_line);
      ppb_dict->Set(dict, line_key, PP_MakeInt32(edit->replace_line.line));
      ppb_dict->Set(dict, text_key, UTF8_TO_VAR((char *)edit->replace_line.text));
      break;
  }
  // Send the message to JS.
  ppb_msg->PostMessage(pp_ins, dict);
  // Clean up leftovers.
  ppb_var->Release(dict);
}

/*
 * The main execution point of this project.
 */
int nacl_main(int argc, char* argv[]) {
  if (setup_unix_environment("vim.tar"))
    return 1;

  // Get the interface variables for manipulating PP_Vars.
  ppb_var = PSInterfaceVar();
  ppb_dict = PSGetInterface(PPB_VAR_DICTIONARY_INTERFACE);
  ppb_msg = PSInterfaceMessaging();
  pp_ins = PSGetInstanceId();
  // Check for missing interfaces.
  if (ppb_var == NULL || ppb_dict == NULL || ppb_msg == NULL) {
    return 2;
  }

  // Create PP_Vars for parsing and creating collabedit JS messages
  type_append_line = UTF8_TO_VAR("append_line");
  type_insert_text = UTF8_TO_VAR("insert_text");
  type_remove_line = UTF8_TO_VAR("remove_line");
  type_delete_text = UTF8_TO_VAR("delete_text");
  type_replace_line = UTF8_TO_VAR("replace_line");
  type_key = UTF8_TO_VAR("collabedit_type");
  line_key = UTF8_TO_VAR("line");
  text_key = UTF8_TO_VAR("text");
  index_key = UTF8_TO_VAR("index");
  length_key = UTF8_TO_VAR("length");

  // Start up message handler loop
  pthread_t looper;
  pthread_create(&looper, NULL, &js_msgloop, NULL);

  // Execute vim's main loop
  return nacl_vim_main(argc, argv);
}

// Print to the js console.
int js_printf(const char* format, ...) {
  char *str;
  int printed;
  va_list argp;

  va_start(argp, format);
  printed = vasprintf(&str, format, argp); 
  va_end(argp);
  if (printed >= 0) {
    // The JS nacl_term just prints any unexpected message to the JS console.
    // We can just send a plain string to have it printed.
    struct PP_Var msg = ppb_var->VarFromUtf8(str, strlen(str));
    ppb_msg->PostMessage(pp_ins, msg);
    ppb_var->Release(msg);
  }
  return printed;
}
