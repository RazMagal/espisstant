/*
 * The spoken-command table. Add phrases freely (synonyms may map to the
 * same intent); MultiNet handles up to ~200 commands. Phrases must be
 * lowercase English words.
 */
#ifndef VOICE_COMMANDS_H
#define VOICE_COMMANDS_H

typedef struct {
    const char *phrase;
    const char *intent; /* must match main/intent.c names */
} voice_command_t;

static const voice_command_t VOICE_COMMANDS[] = {
    { "turn on the lights", "lights_on" },
    { "turn on the light", "lights_on" },
    { "turn off the lights", "lights_off" },
    { "turn off the light", "lights_off" },
    { "dim the lights", "lights_dim" },
    { "brighten the lights", "lights_bright" },
    { "turn on the heater", "heater_on" },
    { "turn off the heater", "heater_off" },
    { "turn on the boiler", "heater_on" },
    { "turn off the boiler", "heater_off" },
    { "turn on the plug", "plug_on" },
    { "turn off the plug", "plug_off" },
    { "turn on the air conditioner", "ac_on" },
    { "turn off the air conditioner", "ac_off" },
    { "make it warmer", "ac_warmer" },
    { "make it cooler", "ac_cooler" },
};

#define VOICE_COMMANDS_COUNT \
    (sizeof(VOICE_COMMANDS) / sizeof(VOICE_COMMANDS[0]))

#endif /* VOICE_COMMANDS_H */
