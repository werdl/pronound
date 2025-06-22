/*
 * pronound - simple pronoun daemon
 * takes a request over the network (a username or uid) and returns the contents of /home/<user>/.pronouns
 * if the file exists, otherwise returns "no pronouns set"
 *
 * pronound is free software distributed under the terms of the GNU General Public License v3.0
 */

#include <stdbool.h>
#include <stdio.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <syslog.h>
#include <stdarg.h>
#include <errno.h>

struct Config {
	bool daemonise;         // whether to run as a daemon, or let itself be handled by a service manager
char *default_pronouns; // default pronouns to return if no pronouns are set
	char *file_path;        // path to the pronouns file from $HOME of user
	int port;               // port to listen on for requests, default is 731
	char *daemon_user;      // user to run the daemon as, default is "_pronound"
};

struct Config config = {.daemonise = false,
                        .default_pronouns = "not specified",
                        .file_path = ".pronouns",
                        .port = 731,
                        .daemon_user = "_pronound"};
int sockfd;
bool daemonised = false;

void error(const char *msg, ...) {
	va_list args;
	va_start(args, msg);
	if (daemonised) {
		syslog(LOG_ERR, "%m"); // %m appends the error message from errno
		vsyslog(LOG_ERR, msg, args);
	} else {
		vprintf(msg, args);
		printf(": %s\n", strerror(errno));
	}
	va_end(args);
}

bool is_number(const char *str) {
	if (*str == '\0')
		return false; // empty string is not a number
	if (*str == '-')
		str++; // skip leading minus sign
	if (*str == '\0')
		return false; // just a minus sign is not a number

	while (*str) {
		if (*str < '0' || *str > '9')
			return false; // non-digit character found
		str++;
	}
	return true; // all characters were digits
}

char *strip(const char *str) {
	while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r')
		str++;
	const char *end = str + strlen(str) - 1;
	while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
		end--;
	size_t len = end - str + 1;
	char *result = malloc(len + 1);
	if (result) {
		strncpy(result, str, len);
		result[len] = '\0';
	}
	return result;
}

uid_t resolve(const char *input, bool *failed) {
	if (is_number(input)) {
		uid_t uid = (uid_t)atoi(input);
		struct passwd *pw = getpwuid(uid);
		if (pw) {
			return pw->pw_uid;
		} else {
			*failed = true;
			return 0; // we can't count on uid_t being signed
		}
	} else {
		struct passwd *pw = getpwnam(input);
		if (pw) {
			return pw->pw_uid;
		} else {
			printf("User %s not found\n", input);
			*failed = true;
			return 0; // we can't count on uid_t being signed
		}
	}
}

char *handle_request(const char *input) {
	bool failed = false;

	uid_t uid = resolve(input, &failed);
	if (failed) {
		return "user not found\n";
	}

	struct passwd *pw = getpwuid(uid);
	if (!pw) {
		return "user not found\n";
	}

	char file_path[256];
	snprintf(file_path, sizeof(file_path), "%s/%s", pw->pw_dir, config.file_path);

	FILE *file = fopen(file_path, "r");
	if (!file) {
		return config.default_pronouns;
	}

	char pronouns[256];
	if (fgets(pronouns, sizeof(pronouns), file)) {
		char *cleaned = strip(pronouns);

		// add newline
		size_t len = strlen(cleaned);
		cleaned[len] = '\n';
		cleaned[len + 1] = '\0';

		fclose(file);
		return cleaned;
	} else {
		fclose(file);
		return config.default_pronouns; // return default if file is empty
	}

	fclose(file);
}

bool drop_privileges(const char *user) {
	struct passwd *pw = getpwnam(user);
	if (!pw) {
		fprintf(stderr, "User %s not found\n", user);
		return false;
	}

	if (setgid(pw->pw_gid) != 0) {
		perror("setgid");
		return false;
	}

	if (setuid(pw->pw_uid) != 0) {
		perror("setuid");
		return false;
	}

	return true;
}

bool split_first_space(const char *str, char **first, char **rest) {
	const char *space = strchr(str, ' ');
	if (!space) {
		*first = strdup(str);
		*rest = NULL;
		return true;
	}

	size_t first_len = space - str;
	*first = malloc(first_len + 1);
	if (!*first)
		return false;

	strncpy(*first, str, first_len);
	(*first)[first_len] = '\0';

	*rest = strdup(space + 1);
	return true;
}

bool parse_config(const char *filename) {
	/*
	 * config file format:
	 * daemonise <true|false>
	 * default_pronouns <pronouns>
	 * file_path <path>
	 * port <port>
	 * daemon_user <user>
	 */

	char *config_file = getenv("PRONOUND_CONFIG");
	if (!config_file)
		config_file = (char *)filename;

	if (!config_file)
		config_file = "/etc/pronound.conf";

	FILE *file = fopen(filename, "r");
	if (!file) {
		perror("Could not open config file");
		return false;
	}

	char line[256];
	while (fgets(line, sizeof(line), file)) {
		char *key, *value;
		char *cleaned_line = strip(line);
		if (strlen(cleaned_line) == 0 || cleaned_line[0] == '#') {
			free(cleaned_line);
			continue; // skip empty lines and comments
		}
		if (!split_first_space(cleaned_line, &key, &value)) {
			free(cleaned_line);
			fclose(file);
			return false; // error splitting line
		}

		if (strcmp(key, "daemonise") == 0) {
			config.daemonise = (value && (strcmp(value, "true") == 0 || strcmp(value, "1") == 0));
		} else if (strcmp(key, "defaults") == 0) {
			// ensure terminated in newline
			char *newline = malloc(strlen(value) + 2);
			if (!newline) {
				free(key);
				free(value);
				free(cleaned_line);
				fclose(file);
				return false;
			}
			strcpy(newline, value);
			newline[strlen(value)] = '\n';
			newline[strlen(value) + 1] = '\0';
			config.default_pronouns = newline;
		} else if (strcmp(key, "file") == 0) {
			config.file_path = strdup(value);
		} else if (strcmp(key, "port") == 0) {
			config.port = atoi(value);
		} else if (strcmp(key, "user") == 0) {
			config.daemon_user = strdup(value);
		}
	}
	return true;
}

void daemonise() {
	pid_t pid = fork();
	if (pid < 0) {
		perror("fork");
		exit(1);
	}
	if (pid > 0) {
		exit(0); // parent exits
	}

	if (setsid() < 0) {
		perror("setsid");
		exit(1);
	}

	umask(0); // clear file mode creation mask
	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	open("/dev/null", O_RDONLY);
	open("/dev/null", O_WRONLY);
}

void handle_signal(int sig) {
	if (sig == SIGINT || sig == SIGTERM) {
		close(sockfd);
		exit(0);
	}
	if (sig == SIGHUP) {
		// Reload configuration if needed
		if (!parse_config("/etc/pronound.conf")) {
			fprintf(stderr, "Failed to reload config file\n");
		}

        if (config.daemonise && !daemonised) {
            daemonised = true;
            daemonise();
        }
	}
}

int main(int argc, char *argv[]) {
	if (getuid() != 0) {
		fprintf(stderr, "pronound must be run as root\n");
		return 1;
	}

	if (!parse_config("/etc/pronound.conf")) {
		fprintf(stderr, "Failed to parse config file\n");
		return 1;
	}

	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	signal(SIGHUP, handle_signal);

	char *config_file = getenv("PRONOUND_CONFIG");
	if (!config_file) {
		config_file = "/etc/pronound.conf";
	}

	bool should_daemonise = false;
	int opt;

	while ((opt = getopt(argc, argv, "dC:")) != -1) {
		switch (opt) {
			case 'd':
				should_daemonise = 1;
				break;
			case 'C':
				config_file = optarg;
				break;
			default:
				fprintf(stderr, "Usage: %s [-d] [-C config_file]\n", argv[0]);
				exit(EXIT_FAILURE);
		}
	}

	openlog("pronound", LOG_PID | LOG_NDELAY, LOG_DAEMON);

	if (config.daemonise || should_daemonise) {
		daemonised = true;
		daemonise();
	}

	// bind to port
	struct addrinfo hints, *res;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;     // IPv4 or IPv6
	hints.ai_socktype = SOCK_STREAM; // TCP socket
	hints.ai_flags = AI_PASSIVE;     // fill in my IP

	char port_str[6];
	snprintf(port_str, sizeof(port_str), "%d", config.port);
	if (getaddrinfo(NULL, port_str, &hints, &res) != 0) {
		error("getaddrinfo failed");
		return 1;
	}

	sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sockfd < 0) {
		error("socket creation failed");
		freeaddrinfo(res);
		return 1;
	}

	int yes = 1;
	if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes) < 0) {
		error("setsockopt failed");
		close(sockfd);
		freeaddrinfo(res);
		return 1;
	}

	if (bind(sockfd, res->ai_addr, res->ai_addrlen) < 0) {
		error("bind failed");
		close(sockfd);
		freeaddrinfo(res);
		return 1;
	}

	drop_privileges(config.daemon_user); // now we are bound to port

	if (listen(sockfd, 5) < 0) {
		error("listen failed");
		close(sockfd);
		freeaddrinfo(res);
		return 1;
	}

	while (true) {
		struct sockaddr_storage client_addr;
		socklen_t addr_len = sizeof(client_addr);
		int client_sock = accept(sockfd, (struct sockaddr *)&client_addr, &addr_len);
		if (client_sock < 0) {
			if (daemonised) {
				syslog(LOG_WARNING, "accept failed %m");
			} else {
				perror("accept");
			}
			continue; // continue to the next iteration on error
		}

		char buffer[256];
		ssize_t bytes_read = read(client_sock, buffer, sizeof(buffer) - 1);
		if (bytes_read < 0) {
			if (daemonised) {
				syslog(LOG_WARNING, "read failed %m");
			} else {
				perror("read");
			}
			close(client_sock);
			continue; // continue to the next iteration on error
		}

		char *clean = strip(buffer);

		char *response = handle_request(clean);

		write(client_sock, response, strlen(response));

		close(client_sock);
	}

	return 0;
}
