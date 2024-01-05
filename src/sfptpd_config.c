/* SPDX-License-Identifier: BSD-3-Clause */
/* (c) Copyright 2012-2019 Xilinx, Inc. */

/**
 * @file   sfptpd_config.c
 * @brief  Command line and configuration file parsing for sfptpd
 */

#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>

#include "sfptpd_logging.h"
#include "sfptpd_config.h"
#include "sfptpd_general_config.h"
#include "sfptpd_constants.h"
#include "sfptpd_sync_module.h"
#include "sfptpd_misc.h"


/****************************************************************************
 * Config File Options
 ****************************************************************************/

/* Long-only options need pseudo identifier */
#define OPT_VERSION   0x10000
#define OPT_NO_DAEMON 0x10001
#define OPT_DAEMON    0x10002
#define OPT_CONSOLE   0x10003

#define CONFIG_REDACTION_STRING "********"

static const char *command_line_options_short = "hf:i:tvu:";
static const struct option command_line_options_long[] = 
{
	{"help", 0, NULL, (int)'h'},
	{"config-file", 1, NULL, (int)'f'},
	{"interface", 1, NULL, (int)'i'},
	{"verbose", 0, NULL, (int)'v'},
	{"user", 1, NULL, (int)'u'},
	{"test-config", 0, NULL, (int)'t'},
	{"version", 0, NULL, OPT_VERSION},
	{"no-daemon", 0, NULL, OPT_NO_DAEMON},
	{"daemon", 0, NULL, OPT_DAEMON},
	{"console", 0, NULL, OPT_CONSOLE},
	{NULL, 0, NULL, 0}
};

static const struct sfptpd_config_option_set *config_options[SFPTPD_CONFIG_CATEGORY_MAX];


/****************************************************************************
 * Config Option Handlers
 ****************************************************************************/


/****************************************************************************
 * Local Functions
 ****************************************************************************/

static void config_make_param_string(char *buffer, unsigned int buffer_size,
				     const char * const tokens[],
				     unsigned int num_tokens)
{
	int len = 0;

	buffer[0] = '\0';
	while ((num_tokens != 0) && (len < buffer_size)) {
		len += snprintf(buffer + len, buffer_size - len,
			        "%s ", tokens[0]);

		tokens++;
		num_tokens--;
	}
}


static void config_display_help(void)
{
	unsigned int i, j;
	const struct sfptpd_config_option_set *set;

	printf(
		"\nUsage:  sfptpd -i <interface> [OPTION]\n\n"
		"Version: %s\n"
		"\n"
		"Command Line Options:\n"
		"-h, --help                   Display help information\n"
		"-i, --interface=INTERFACE    Default interface that Synchronization Modules will use\n"
		"-f, --config-file=FILE       Configure from FILE, or stdin if '-'\n"
		"-t, --test-config            Test configuration\n"
		"-u, --user=USER[:GROUP]      Run as user USER (and group GROUP)\n"
		"    --no-daemon              Do not run as a daemon, overriding config file\n"
		"    --daemon                 Run as a daemon, overriding config file\n"
		"-v, --verbose                Verbose: enable stats, trace and send output to stdout/stderr\n"
		"    --console                Send output to stdout/stderr\n"
		"    --version                Show version number and exit\n"
		"\n"
		"Runtime Signals:\n"
		"SIGHUP              Rotate message and statistics log (if logging to file)\n"
		"SIGUSR1             Step the clocks by the current offset from the master clock\n"
		"\n",
		SFPTPD_VERSION_TEXT
	);

	for (i = 0; i < sizeof(config_options)/sizeof(config_options[0]); i++) {
		set = config_options[i];

		printf("%s:\n", set->description);

		for (j = 0; j < set->num_options; j++) {
			if (!set->options[j].hidden) {
				printf("%-28s %-30s %s\n",
				       set->options[j].option,
				       set->options[j].params,
				       set->options[j].description);
			}
		}

		printf("\n");
	}
}


/* This function is declared global to allow unit testing */
unsigned int tokenize(char *input, unsigned int max_tokens, char *tokens[])
{
	unsigned int token_count = 0;
	char *token = NULL;
	bool escaped = false;
	bool single_quotes = false;
	bool double_quotes = false;
	char c;

	while (true) {
		c = *input;
		
		/* When we get a null termination, exit whatever the state */
		if (c == '\0') 
			break;

		/* If not escaped and we get an escape character, prepare to
		 * escape the next character */
		if (!escaped && (c == '\\')) {
			escaped = true;
		} else if (token == NULL) {
			/* We are currently not in a token. */
			if (single_quotes || double_quotes) {
				/* If we have previously had single or double
				 * quotes start a token regardless of the
				 * character */
				token = input;
				/* If not escaped then handle the case where
				 * we have open quotes immediately followed
				 * by close quotes */
				if (!escaped &&
				    ((single_quotes && (c == '\'')) ||
				     (double_quotes && (c == '\"')))) {
					*input = '\0';
					single_quotes = false;
					double_quotes = false;
					/* If we have used all the token slots
					 * stop processing */
					tokens[token_count] = token;
					token_count++;
					token = NULL;
					if (token_count == max_tokens)
						break;
				}
			} else if (!escaped && (c == '\'')) {
				/* Start of a quoted parameter. Whatever it is,
				 * the next character is the start of a token */
				single_quotes = true;
			} else if (!escaped && (c == '\"')) {
				/* Start of a quoted parameter. Whatever it is,
				 * the next character is the start of a token */
				double_quotes = true;
			} else if (!escaped && ((c == '\n') || (c == '#'))) {
				/* Outside a token, a carriage return or 
				 * comment start will cause us to stop
				 * processing immediately. Inside a token
				 * we ignore comment characters. */
				break;
			} else if ((c != ' ') && (c != '\t')) {
				/* By default, non-whitespace is taken as the
				 * start of a token */
				token = input;
			}

			escaped = false;
		} else {
			/* We are in a token. Handle escape characters.
			 * Otherwise, if we get close quotes or white space
			 * when not quoted, this is the end of the token */
			if (escaped) {
				/* We are escaped and in a token, so move all
				 * the token character so far up one */
				memmove(token + 1, token, input - token - 1);
				token++;
				escaped = false;
			} else if ((single_quotes && (c == '\'')) ||
				   (double_quotes && (c == '\"')) ||
				   (!single_quotes && !double_quotes &&
				    ((c == ' ') || (c == '\t') || (c == '\n') || (c == '#')))) {
				single_quotes = false;
				double_quotes = false;
				/* Null terminate the current token */
				*input = '\0';
				tokens[token_count] = token;
				token_count++;
				token = NULL;
				/* If we have used all the token slots or the
				 * character is a carriage return or start of
				 * comment, stop processing. */
				if ((c == '\n') || (c == '#') ||
				    (token_count == max_tokens))
					break;
			}
		}

		/* Next char */
		input++;
	}
	
	/* We may be in a token - null terminate it. */
	if (token) {
		/* If we are escaped when we exit then we need to remove the
		 * trailing backslash */
		if (escaped)
			input--;
		*input = '\0';
		tokens[token_count] = token;
		token_count++;
	}

	return token_count;
}


static void convert_dashes_to_underscores(char *token)
{
	assert(token != NULL);

	for( ; *token != '\0'; token++) {
		if (*token == '-')
			*token = '_';
	}
}


static int config_syntax_check(unsigned int num_tokens, char *tokens[])
{
	char *token, *open_sqr, *close_sqr;

	assert(num_tokens > 0);
	assert(tokens != NULL);

	/* The first token should not contain square brackets unless it is a
	 * section definition and in this case the first character should be [
	 * and the last ]. */
	token = tokens[0];
	open_sqr = strrchr(token, '[');
	close_sqr = strchr(token, ']');

	if (((open_sqr != NULL) && (open_sqr != token)) ||
	    ((close_sqr != NULL) && (close_sqr != token + strlen(token) - 1))) {
		ERROR("config: unexpected square brackets in configuration line \'%s\'.\n",
		      token);
		return EINVAL;
	}

	return 0;
}


/* Parse config option.
 * Returns: 1 for a confidential option,
 *          -errno on error,
 *          0 otherwise.
 */
static int config_parse_option(struct sfptpd_config *config,
			       struct sfptpd_config_section *section,
			       unsigned int num_tokens,
			       const char * const tokens[])
{
	const sfptpd_config_option_t *options, *opt;
	unsigned int num_options, i;
	int rc;
	char params[SFPTPD_CONFIG_LINE_LENGTH_MAX];
	unsigned int num_params;

	/* We expect to be called with a valid config and more than 0 tokens */
	assert(section != NULL);
	assert(tokens != NULL);
	assert(num_tokens > 0);
	assert(section->category < SFPTPD_CONFIG_CATEGORY_MAX);

	/* Make a string from the parameters for error and trace logging */
	num_params = num_tokens - 1;
	config_make_param_string(params, sizeof(params), tokens + 1, num_params);

	/* Find the config options for this category. If there aren't any then
	 * return no-entry */
	if (config_options[section->category] == NULL)
		return -ENOENT;

	num_options = config_options[section->category]->num_options;
	options = config_options[section->category]->options;

	for (i = 0; i < num_options; i++) {
		opt = &options[i];

		/* If the option name matches the token, parse the option. */
		if (strcmp(tokens[0], opt->option) == 0) {
			/* If the option is a global option, then we should not
			 * be trying to parse it in an instance section. */
			if ((opt->scope == SFPTPD_CONFIG_SCOPE_GLOBAL) &&
			    (section->scope == SFPTPD_CONFIG_SCOPE_INSTANCE)) {
				ERROR("global configuration option \'%s\' cannot "
				      "be used in instance configuration \'%s\'\n",
				      opt->option, section->name);
				return -EINVAL;
			}

			/* Are there an appropriate number of parameters? */
			bool exact_reqd = opt->num_params >= 0;
			int num_reqd = exact_reqd ? opt->num_params : ~opt->num_params;

			if ((exact_reqd && (num_params != num_reqd)) ||
			     (!exact_reqd && (num_params < num_reqd))) {

				CFG_ERROR(section, "option %s expects %s %d "
					  "parameter%s but have %d%c %s\n",
					  opt->option,
					  exact_reqd ? "exactly": "at least",
					  num_reqd,
					  num_reqd == 1 ? "" : "s",
					  num_params,
					  (num_params > 0)? ',': ' ', params);
				return -EINVAL;
			}

			/* Parse the option! */
			rc = opt->parse(section, opt->option,
					num_params, tokens + 1);

			if (rc == EINVAL) {
				CFG_ERROR(section, "option %s expects %s, but have %s\n",
					  opt->option, opt->params, params);
				return -rc;
			} else if (rc != 0) {
				CFG_ERROR(section, "failed to parse %s %s, error %s\n",
					  opt->option, params, strerror(rc));
				return -rc;
			}

			TRACE_L2("config [%s]: %s %c %s\n",
				 section->name, opt->option,
			         (num_params > 0)? '=': ' ', opt->confidential ? CONFIG_REDACTION_STRING : params);

			// TODO ideally want to not overwrite if already
			// assigned in instance!!!
			/* If this is a global option, apply it to all the
			 * instance sections in the same category */
			if (section->scope == SFPTPD_CONFIG_SCOPE_GLOBAL) {
				struct sfptpd_config_section *s;

				for (s = sfptpd_config_category_first_instance(config, section->category);
				     s != NULL;
				     s = sfptpd_config_category_next_instance(s)) {
					rc = opt->parse(s, opt->option,
							num_params, tokens + 1);
					assert(rc == 0);
					TRACE_L3("config [%s]: %s %c %s\n",
						 s->name, opt->option,
						 (num_params > 0)? '=': ' ', opt->confidential ? CONFIG_REDACTION_STRING : params);
				}
			}

			return opt->confidential ? 1 : 0;
		}
	}

	ERROR("config [%s]: option %s not found\n", section->name, tokens[0]);
	return -ENOENT;
}


static char *config_is_new_section(char *token)
{
	size_t len;
	assert(token != NULL);

	len = strlen(token);

	/* Is this a section definition in the form [section-name]? */
	if ((token[0] == '[') && (token[len - 1] == ']')) {
		/* This is a section defintion. Remove the brackets. */
		token[len - 1] = '\0';
		token++;
		len -= 2;
		return token;
	}

	return NULL;
}


/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sfptpd_config_create(struct sfptpd_config **config)
{
	struct sfptpd_config *new;
	int rc;

	assert(config != NULL);

	/* Initialise the configuration sets */
	memset(&config_options, 0, sizeof(config_options));

	new = calloc(1, sizeof(*new));
	if (new == NULL) {
		CRITICAL("failed to allocate memory for configuration\n");
		return ENOMEM;
	}

	/* Create general configuration section */
	rc = sfptpd_general_config_init(new);
	if (rc != 0)
		goto fail;

	/* Create top-level configurations for each sync module */
	rc = sfptpd_sync_module_config_init(new);
	if (rc != 0)
		goto fail;

	*config = new;
	return 0;

fail:
	sfptpd_config_destroy(new);
	return rc;
}


void sfptpd_config_destroy(struct sfptpd_config *config)
{
	sfptpd_config_section_t *s;
	unsigned int i;
	
	assert(config != NULL);

	/* For each category, go through the linked list deleting each section */
	for (i = 0; i < SFPTPD_CONFIG_CATEGORY_MAX; i++) {
		while (config->categories[i] != NULL) {
			/* Unlink and delete the section */
			s = config->categories[i];
			config->categories[i] = s->next;

			s->ops.destroy(s);
		}
	}

	/* Finally free the overall config structure. */
	free(config);
}


void sfptpd_config_register_options(const struct sfptpd_config_option_set *options)
{
	assert(options != NULL);
	assert(config_options[options->category] == NULL);
	config_options[options->category] = options;
}


void sfptpd_config_section_init(struct sfptpd_config_section *section,
				sfptpd_config_section_create_t create,
				sfptpd_config_section_destroy_t destroy,
				enum sfptpd_config_category category,
				enum sfptpd_config_scope scope,
				bool allows_instances,
				const char *name)
{
	assert(section != NULL);
	/* No create op is required for instances */
	assert((create != NULL) || (scope == SFPTPD_CONFIG_SCOPE_INSTANCE));
	assert(destroy != NULL);
	assert(category < SFPTPD_CONFIG_CATEGORY_MAX);
	assert(scope < SFPTPD_CONFIG_SCOPE_MAX);
	/* We only expect instances to be possible for global sections */
	assert(!allows_instances || (scope == SFPTPD_CONFIG_SCOPE_GLOBAL));
	assert(name != NULL);
	assert(strlen(name) < SFPTPD_CONFIG_SECTION_NAME_MAX);

	/* Initialise the header */
	section->ops.create = create;
	section->ops.destroy = destroy;
	section->next = NULL;
	section->config = NULL;
	section->category = category;
	section->scope = scope;
	section->allows_instances = allows_instances;
	sfptpd_strncpy(section->name, name, sizeof(section->name));
}


void sfptpd_config_section_add(struct sfptpd_config *config,
			       struct sfptpd_config_section *section)
{
	struct sfptpd_config_section **iter;

	assert(config != NULL);
	assert(section != NULL);

	/* At this point we expect that there is no section with this name
	 * already in the config. By implication this also guarantees that
	 * this section is not being added for a second time. */
	assert(section->next == NULL);
	assert(sfptpd_config_find(config, section->name) == NULL);

	/* If this is the global section, it should be the first. */
	assert(section->category < SFPTPD_CONFIG_CATEGORY_MAX);
	assert((section->scope == SFPTPD_CONFIG_SCOPE_INSTANCE) ||
	       (config->categories[section->category] == NULL));

	/* Add a reference from the section back to the overall config and
	 * set the scope and instances. */
	section->config = config;

	/* Add section to the end of the linked list for the category */
	for (iter = &config->categories[section->category]; *iter != NULL;
	     iter = &(*iter)->next);
	*iter = section;
}


struct sfptpd_config *sfptpd_config_top_level(struct sfptpd_config_section *section)
{
	assert(section != NULL);
	return section->config;
}


struct sfptpd_config_section *sfptpd_config_category_global(struct sfptpd_config *config,
							     enum sfptpd_config_category category)
{
	assert(config != NULL);
	assert(category < SFPTPD_CONFIG_CATEGORY_MAX);
	return config->categories[category];
}


struct sfptpd_config_section *sfptpd_config_category_first_instance(struct sfptpd_config *config,
								enum sfptpd_config_category category)
{
	assert(config != NULL);
	assert(category < SFPTPD_CONFIG_CATEGORY_MAX);
	if (config->categories[category] != NULL)
		return config->categories[category]->next;
	return NULL;
}


struct sfptpd_config_section *sfptpd_config_category_next_instance(struct sfptpd_config_section *section)
{
	assert(section != NULL);
	return section->next;
}


int sfptpd_config_category_count_instances(struct sfptpd_config *config,
					   enum sfptpd_config_category category) {
	struct sfptpd_config_section *ptr;
	int count = 0;

	assert(config != NULL);
	assert(category < SFPTPD_CONFIG_CATEGORY_MAX);
	ptr = config->categories[category];
	assert(ptr);
	for (ptr = config->categories[category]->next; ptr; ptr = ptr->next) count++;
	return count;
}


struct sfptpd_config_section *sfptpd_config_find(struct sfptpd_config *config,
						 const char *name)
{
	struct sfptpd_config_section *s;
	unsigned int i;

	assert(config != NULL);
	assert(name != NULL);

	for (i = 0; i < SFPTPD_CONFIG_CATEGORY_MAX; i++) {
		for (s = config->categories[i]; s != NULL; s = s->next) {
			if (strcmp(s->name, name) == 0)
				return s;
		}
	}

	return NULL;
}


const char *sfptpd_config_get_name(struct sfptpd_config_section *section)
{
	assert(section != NULL);

	return section->name;
}


int sfptpd_config_parse_command_line_pass1(struct sfptpd_config *config,
					   int argc, char **argv)
{
	int chr, index, rc = 0;
	char *group;
	assert(config != NULL);
	assert(argv != NULL);

	/* parse command line arguments */
	optind = 1;
	while ((chr = getopt_long(argc, argv, command_line_options_short,
				  command_line_options_long, &index)) != -1) {
		switch (chr) {
		case 'h':
			config_display_help();
		case OPT_VERSION:
			/* Terminate early: not an error */
			return ESHUTDOWN;
			break;

		case 'f':
			sfptpd_config_set_config_file(config, optarg);
			break;

		case 'v':
			sfptpd_config_general_set_verbose(config);
			break;

		case OPT_CONSOLE:
			sfptpd_config_general_set_console_logging(config);
			break;

		case 'i':
			/* Update the interface name for the global section of
			 * each sync module. */
			sfptpd_sync_module_set_default_interface(config, optarg);
			break;

		case 'u':
			if ((group = strchr(optarg, ':')))
				*group++ = '\0';
			if ((rc = sfptpd_config_general_set_user(config, optarg, group)))
				return rc;
			break;

		case 't':
		case OPT_NO_DAEMON:
		case OPT_DAEMON:
			/* Handled in second pass */
			break;

		case '?':
		default:
			/* Print out the offending command line option */
			ERROR("unrecognised option \"%s\"\n", argv[optind - 1]);
			return EINVAL;
			break;
		}
	}

	if (optind < argc) {
		printf("expected a command line option, got \"%s\"\n", argv[optind]);
		return EINVAL;
	}

	return rc;
}


int sfptpd_config_parse_command_line_pass2(struct sfptpd_config *config,
					   int argc, char **argv)
{
	int chr, index;
	assert(config != NULL);
	assert(argv != NULL);

	/* parse command line arguments */
	optind = 1;
	while ((chr = getopt_long(argc, argv, command_line_options_short,
				  command_line_options_long, &index)) != -1) {
		switch (chr) {
		case 'h':
		case 'f':
		case 'i':
		case 'u':
		case OPT_VERSION:
			/* We've already handled these- ignore them */
			break;

		case OPT_NO_DAEMON:
			sfptpd_config_general_set_daemon(config, false);
			break;

		case OPT_DAEMON:
			sfptpd_config_general_set_daemon(config, true);
			break;

		case 'v':
			sfptpd_config_general_set_verbose(config);
			break;

		case OPT_CONSOLE:
			sfptpd_config_general_set_console_logging(config);
			break;

		case 't':
			/* Terminate early: not an error */
			return ESHUTDOWN;

		case '?':
		default:
			/* Print out the offending command line option */
			ERROR("unrecognised option \"%s\"\n", argv[optind - 1]);
			return EINVAL;
			break;
		}
	}

	return 0;
}


int sfptpd_config_parse_file(struct sfptpd_config *config)
{
	FILE *cfg_file;
	struct stat file_stat;
	char line[SFPTPD_CONFIG_LINE_LENGTH_MAX];
	char *tokens[SFPTPD_CONFIG_TOKENS_MAX];
	sfptpd_config_section_t *section;
	unsigned int num_tokens;
	char *section_name;
	struct sfptpd_config_general *general_config;
	int rc;

	assert(config != NULL);

	/* For backward compatibility we default to the 'general' section of
	 * the configuration file. */
	general_config = sfptpd_general_config_get(config);
	section = NULL;

	/* If no config file has been specified, return success. */
	if (general_config->config_filename[0] == '\0') {
		TRACE_L4("no config file specified\n");
		return 0;
	}

	if (!strcmp(general_config->config_filename, "-")) {
		int fd = dup(STDIN_FILENO);

		if (fd == -1) {
			rc = errno;
			ERROR("dup() on stdin, %s\n", strerror(rc));
			return rc;
		}

		cfg_file = fdopen(fd, "r");
	} else {
		rc = stat(general_config->config_filename, &file_stat);
		if (rc < 0) {
			ERROR("failed to retrieve info on config file, %s\n", strerror(errno));
			return rc;
		}

		if (S_ISDIR(file_stat.st_mode)) {
			ERROR("config file is a directory\n");
			return ENOENT;
		}

		cfg_file = fopen(general_config->config_filename, "r");
	}

	if (cfg_file == NULL) {
		rc = errno;
		ERROR("failed to open config file %s, error %d\n",
		      general_config->config_filename, rc);
		return rc;
	}

	sfptpd_log_lexed_config("# Reconstructed from: %s\n",
				general_config->config_filename);

	while (fgets(line, sizeof(line), cfg_file) != NULL) {
		/* Tokenize the string */
		num_tokens = tokenize(line, sizeof(tokens)/sizeof(tokens[0]), tokens);
		if (num_tokens != 0) {
			/* For the first token i.e. the parameter being set,
			 * convert dashes to underscores */
			convert_dashes_to_underscores(tokens[0]);

			/* Validate the token syntax */
			rc = config_syntax_check(num_tokens, tokens);
			if (rc != 0)
				return rc;

			/* Does this configuration line specify a new section? */
			section_name = config_is_new_section(tokens[0]);
			if (section_name != NULL) {

				/* Validate the section just implicitly closed. */
				if (section && config_options[section->category]->validator &&
				    (rc = config_options[section->category]->validator(section)) != 0)
					return rc;

				/* Find the new section- if this doesn't
				 * succeed then the file syntax is bad */
				section = sfptpd_config_find(config, section_name);
				if (section == NULL) {
					ERROR("configuration section \'%s\' doesn't exist\n",
					      section_name);
					return ENOENT;
				}

				TRACE_L3("config: entering section \'%s\'\n",
					 section_name);
				sfptpd_log_lexed_config("\n[%s]\n", section_name);
			} else if (section != NULL) {
				int i;

				/* Parse the option. The function returns ENOENT
				 * if the option is not found or 0 for success.
				 * Anything else is treated as an error.
				 *
				 * The irritating cast is necessary because of
				 * this:
				 *   http://c-faq.com/ansi/constmismatch.html
				 */
				rc = config_parse_option(config, section, num_tokens,
							 (const char * const *)tokens);
				if (rc < 0)
					return -rc;

				for (i = 0; i < num_tokens; i++) {
					sfptpd_log_lexed_config("%s%c",
								i > 0 && rc == 1 ? CONFIG_REDACTION_STRING : tokens[i],
								(i < num_tokens - 1) ? ' ' : '\n');
				}
			} else {
				ERROR("config: stanza specified outside a section\n");
				return EINVAL;
			}
		}
	}

	/* Validate the last section. */
	if (config_options[section->category]->validator &&
	    (rc = config_options[section->category]->validator(section)) != 0)
		return rc;

	fclose(cfg_file);
	return 0;
}


/* fin */
