/* br_playtest.h - play-testing for agents and regression tests: drive a game with scripted or
 * interactive input, take screenshots, read and write state. docs/PLAYTESTING.md has the recipe
 * and the full command reference.
 *
 * Every br_app executable has it, off unless asked for:
 *   game --playtest script.json     run a script (a JSON array of commands), then exit:
 *                                   0 = every expect passed, 1 = a step failed (logged)
 *   game --playtest-stdin           one JSON command per line on stdin, one JSON reply per line
 *                                   on stdout; the game advances only on "step" / "wait"
 *   game --record session.json      write the real input of this run as a script at exit
 * --playtest and --playtest-stdin imply --deterministic and default to --vsync off.
 *
 * A command is an object { "cmd": "...", ... }, the same in scripts and on stdin:
 *   { "cmd": "step", "frames": 60 }                      advance (also "wait"; "seconds" too)
 *   { "cmd": "key", "key": "Space" }                     tap (down now, up next frame);
 *                                                        "action": "down"/"up", "frames": hold
 *   { "cmd": "action", "name": "start" }                 press a game.json input action
 *   { "cmd": "click", "label": "play_button" }           click the centre of a node
 *   { "cmd": "get", "path": "ship:position" }            node property / component field
 *   { "cmd": "set", "path": "game:lives", "value": 1 }   force a state
 *   { "cmd": "expect", "path": "game:score", "op": ">=", "value": 100 }
 *   { "cmd": "screenshot", "file": "out.png" }  { "cmd": "dump", "file": "state.json" }
 *
 * Paths: "label:property" (position, x, y, scale, rotation, alpha, visible, tint, z_index,
 * world_position, bounds, type, children, exists, or any key the node's scene-file form has,
 * e.g. "text"), "label:component.field", a trailing ".x"/".y"/".z"/".w" or ".0", ".1"... picks
 * one element, and "name:field" for structs a game exposes with br_playtest_expose.
 */
#ifndef BR_PLAYTEST_H
#define BR_PLAYTEST_H

#include "braty/br_app.h"
#include "braty/br_node.h"

#include "cjson/cJSON.h"

/* Creates the play-test state. Called by the engine; games never call it. */
void br_playtest_register(br_engine *e);

/* ---- scripts */

/* Loads and checks a script (errors name the step). It runs from the next frame, one frame per
 * br_engine_step; when it ends the app quits (exit code 1 if a step failed). */
bool br_playtest_load_script(br_engine *e, const char *path);
/* Headless convenience: loads a script and steps the engine until it ends. True = passed. */
bool br_playtest_run_script(br_engine *e, const char *path);
/* True while a script is loaded and not finished. */
bool br_playtest_running(br_engine *e);
/* True once a script step or command failed (expect, wait_until timeout, bad path...). */
bool br_playtest_failed(br_engine *e);
/* The reason of the last failure ("round.json: step 3: unknown command 'kye'"), "" if none. */
const char *br_playtest_last_error(br_engine *e);

/* ---- interactive (what --playtest-stdin does with each line) */

/* Runs one command (a JSON object as text) and returns the reply line, e.g.
 * {"ok":true,"frame":120,"value":[640,360]}. Free it with br_free. "step" advances the engine
 * before replying. */
char *br_playtest_command(br_engine *e, const char *json_line);

/* ---- the same things as plain C calls */

/* Steps `frames` frames of 1/60 s (renders when there is a window). */
void br_playtest_step(br_engine *e, uint32_t frames);
/* Writes the last rendered frame as PNG (same as br_engine_screenshot). */
bool br_playtest_screenshot(br_engine *e, const char *path);
/* Scene tree + components + exposed state + stats as JSON text (free with br_free). */
char *br_playtest_dump_json(br_engine *e);
/* Reads a value by path; NULL (logged) if the path does not resolve. Free with cJSON_Delete. */
cJSON *br_playtest_get(br_engine *e, const char *path);
/* Writes a value by path. False (logged) if the path or the value is wrong. */
bool br_playtest_set(br_engine *e, const char *path, const cJSON *value);
/* Makes a game struct readable and writable as "name:field" (score, lives, state...). `base`
 * must stay valid while the engine runs; fields as for components. */
void br_playtest_expose(br_engine *e, const char *name, const br_field_desc *fields, uint32_t count, void *base);

/* ---- record and replay */

/* Starts recording real input (keyboard, mouse, text, touch) frame by frame. */
bool br_playtest_record_start(br_engine *e, const char *path);
/* Writes the recording as a script (waits + input commands + a final wait and quit). */
bool br_playtest_record_stop(br_engine *e);

#endif
