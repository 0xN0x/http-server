#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "check.h"

void *handle_client(void *);
void handle_200(int sockfd, FILE* file);
void handle_403(int sockfd);
void handle_404(int sockfd);
void handle_500(int sockfd);
char* get_http_date();
int get_int_length(int value);
long fsize(FILE* fd);


int client_socket;

struct client_element_s {
    struct sockaddr client_addr;
    struct sockaddr_in *client_addr_v4;
    socklen_t client_addr_len;
    int *client_socket_heap;
};

typedef struct {
    struct client_element_s client;
    pthread_mutex_t lock;
} client_t;

char CWD[PATH_MAX];

int main(int argc, char *const argv[]) {
    if (argc != 2) {
        printf("Usage: %s PORT\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in sin;
    // Quel type d'adresse fait référence la structure ?
    sin.sin_family = AF_INET;

    // On parse le numéro de port avec sscanf pour pouvoir détecter les erreurs
    unsigned short port;
    if (sscanf(argv[1], "%hu", &port) != 1) {
        fprintf(stderr, "Bad port number %s\n", argv[2]);
        exit(EXIT_FAILURE);
    }

    // On crée notre socket `AF_INET` (ipv4) en TCP (`SOCK_STREAM`)
    int sock = -1;
    CHK_NEG(sock = socket(sin.sin_family, SOCK_STREAM, 0));

    // Port d'écoute
    // On converti le numéro de port vers BIG-ENDIAN
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);

    // On spécifie au kernel notre port et adresse d'écoute. 0 signifie choisi pour moi.
    // Si on ne fait pas l'appel à `bind` c'est le kernel qui va tout choisir
    CHK_NEG(bind(sock, (const struct sockaddr *)&sin, sizeof(sin)));

    // On 'active' notre socket en la mettant en écoute passive.
    CHK_NEG(listen(sock, 1));

    while (1) {
        client_t client;
        client.client.client_addr_len =  sizeof(client.client.client_addr);
        /* socklen_t client_addr_len = sizeof(client_addr); */
        // On attend une nouvelle connexion d'un client. Le kernel va nous remplir les variables
        // `client_addr` et `client_addr_len` et nous renvoyer un FD (File Descriptor)
        // correspondant à la connexion/socket entre ce client et notre serveur.
        pthread_mutex_lock(&client.lock);
        CHK_NEG(client_socket = accept(sock, &client.client.client_addr, &client.client.client_addr_len));
        pthread_mutex_unlock(&client.lock);
        switch (client.client.client_addr.sa_family) {
            case AF_INET:
                // On cast notre structure sockaddr dans le bon d'après la `famille`
                pthread_mutex_lock(&client.lock);
                client.client.client_addr_v4 = (struct sockaddr_in *)&client.client.client_addr;
                printf("New connection from %s:%d\n", inet_ntoa(client.client.client_addr_v4->sin_addr),
                       /* On n'oublie pas que le `sin_port` est en BIG-ENDIAN */
                       ntohs(client.client.client_addr_v4->sin_port));
                pthread_mutex_unlock(&client.lock);
                break;
            default:
                fprintf(stderr, "Unsupported sa_family\n");
                exit(EXIT_FAILURE);
        }

        pthread_mutex_lock(&client.lock);
        client.client.client_socket_heap = NULL;
        pthread_mutex_unlock(&client.lock);
        // On crée une zone mémoire par client afin d'éviter que tous les arguments `ptr` de
        // `handle_client` ne pointent vers la même adresse mémoire.
        pthread_mutex_lock(&client.lock);
        CHK_NULL(client.client.client_socket_heap = (int *)malloc(sizeof(int)));
        pthread_mutex_unlock(&client.lock);

        pthread_mutex_lock(&client.lock);
        *client.client.client_socket_heap = client_socket;
        pthread_mutex_unlock(&client.lock);

        // On crée un thread pour ce client
        // * tid sera la référence du thread créé.
        // * NULL: attributs du threads, NULL signifie que la fonction doit utiliser les paramètres
        // par défaut.
        // * handle_client : fonction exécutée par le nouveau dès sa création
        // * &client_socket : paramètre de la fonction exécutée par le thread
        pthread_t tid;
        pthread_mutex_lock(&client.lock);
        CHK_NEG(pthread_create(&tid, NULL, handle_client, client.client.client_socket_heap));
        pthread_mutex_unlock(&client.lock);
    }
    CHK_NEG(close(sock));

    return EXIT_SUCCESS;
}

void *handle_client(void *ptr) {
    client_socket = *(int *)ptr;
    free(ptr);
    printf("Thread %lx: arg=%p, sock=%d \n", pthread_self(), ptr, client_socket);
    char buffer[BUFSIZ];

    // Boucle infinie
    while (1) {
        // On essaie de recevoir des données du serveur, jusqu'à BUFSIZ (8192)
        ssize_t n = recv(client_socket, buffer, sizeof(buffer), 0);
        if (n < 0) {
            perror("recv");
            break;
        } else if (n == 0) {            
            printf("Remote end is done\n");
            break;
        }

        // [TODO] sscanf with method not working, returning nothing
        char method[10];
        char path[2048];
        char version[8];

        if (sscanf(buffer, "%s %s %s[^\r\n]*c", method, path, version) != 3) {
            // [TODO] Add perror when method fixed
            printf("%d\n", errno);
        }

        // Chemin par défaut si / est fourni
        if (strlen(path) == 1 && path[0] == '/') {
            strncpy(path, "/index.html", 2048);
        }

        // Concaténation du caractère '.' avec le chemin demandé
        char tmp[strlen(path) + 1];
        memset(tmp,0,strlen(tmp));
        tmp[0] = '.';
        strcat(tmp, path);

        printf("User requested path \"%s\"\n", path);

        errno = 0;
        FILE *file = fopen(tmp, "r+");

        switch (errno) {
            case 0: // File found
                printf("Responding HTTP 200\n");
                handle_200(client_socket, file);
                break;
            case EACCES: // Can't access
                printf("Responding HTTP 403\n");
                handle_403(client_socket);
                break;
            case EISDIR: // Is a directory
            case ENOENT: // Element does not exist or is a dangling symbolic link
                printf("Responding HTTP 404\n");
                handle_404(client_socket);
                break;
            default: // Unhandled error
                printf("Responding HTTP 500\n");
                handle_500(client_socket);
                break;
        }

        if (n < 0) {
            perror("send");
            break;
        }

        //fclose(file);
    }
    // On a terminé, on la ferme pour libérer les ressources.
    close(client_socket);

    return NULL;
}

/*
 * Function:  handle_200
 * --------------------
 * Default handler when the request is successful
 *
 *  int sockfd   : file descriptor of the socket
 *  int filefd   : file descriptor of the file to send
 *
 *  returns: nothing
 */
void handle_200(int sockfd, FILE *file) {
    // Initialisation des variables
	long content_length, n;
	long filesize = fsize(file);
	char *buf;
	char *date;
	char *file_content;

    // Allocation de la variable utilisée pour stocker le contenu du fichier
	file_content = (char *)malloc(filesize * sizeof(char));

    // Récupération de la date dans le format demandé par la RFC de HTTP
    // (cf. rfc2616 - section-3.3.1)
	date = get_http_date();

    // Lecture du fichier
	n = fread(file_content, sizeof(char), filesize, file);
    if (n < 0) {
        perror("fread");
        handle_500(sockfd);
        return;
    }

    // Calcul de la taille du buffer (95 octets de header + la taille du 
    // fichier + le nombre de caractère de l'entier précédémment calculé)
	content_length = filesize + 95;
	content_length += get_int_length(content_length);

    // Allocation de la variable utilisée pour stocker la réponse HTTP
	buf = (char *)malloc(content_length * sizeof(char));


    // Création du buffer de réponse
	snprintf(
        buf,
        content_length,
		"HTTP/1.1 200 OK\n"
		"Date: %s\n"
		"Content-Type: text/html\n"
		"Content-Length: %ld\n\n"
		"%s",
		date,
		filesize,
		file_content
	);

    // Envoi de la réponse HTTP au client
    n = send(sockfd, buf, strlen(buf), 0);
    if (n < 0) {
        perror("send");
    }

    // Libération de la mémoire
	free(date);
	free(file_content);
    free(buf);
}

/*
 * Function:  handle_4043
 * --------------------
 * Handler when the requested file isn't accessible
 *
 *  int sockfd    : socket file descriptor
 *
 *  returns:
 */
void handle_403(int sockfd) {
    // Initialisation des variables
	// Remplacer 119 par 120 avant l'an 10000
	char buf[130];

    // Récupération de la date dans le format demandé par la RFC de HTTP
    // (cf. rfc2616 - section-3.3.1)
	char* date = get_http_date();

    // Création du buffer de réponse
	sprintf(buf,
		"HTTP/1.1 403 Forbidden\n"
		"Date: %s\n"
		"Content-Type: text/plain\n"
		"Content-Length: 9\n\n"
		"Forbidden\n",
		date
	);

    // Envoi de la réponse HTTP au client
	int n = send(sockfd, buf, strlen(buf), 0);
    if (n < 0) {
        perror("send");
    }

    // Libération de la mémoire
    free(date);
}

/*
 * Function:  handle_404 
 * --------------------
 * Handler when the requested file doesn't exist 
 *
 *  int sockfd    : socket file descriptor
 *
 *  returns:
 */
void handle_404(int sockfd) {
    // Initialisation des variables
	// Remplacer 119 par 120 avant l'an 10000
	char buf[130];

    // Récupération de la date dans le format demandé par la RFC de HTTP
    // (cf. rfc2616 - section-3.3.1)
	char* date = get_http_date();

    // Création du buffer de réponse
	sprintf(buf,
		"HTTP/1.1 404 Not Found\n"
		"Date: %s\n"
		"Content-Type: text/plain\n"
		"Content-Length: 14\n\n"
		"Page not found\n",
		date
	);

    // Envoi de la réponse HTTP au client
	int n = send(sockfd, buf, strlen(buf), 0);
    if (n < 0) {
        perror("send");
    }

    // Libération de la mémoire
    free(date);
}

/*
 * Function:  handle_500
 * --------------------
 * Default route for server error (permission error to access file, path too long, etc..)
 *
 *  int sockfd    : socket file descriptor
 *
 *  returns:
 */
void handle_500(int sockfd) {
    // Initialisation des variables
	char buf[138];

    // Récupération de la date dans le format demandé par la RFC de HTTP
    // (cf. rfc2616 - section-3.3.1)
	char* date = get_http_date();

    // Création du buffer de réponse
	sprintf(buf,
		"HTTP/1.1 500 Internal Server Error\n"
		"Date: %s\n"
		"Content-Type: text/plain\n"
		"Content-Length: 21\n\n"
		"Internal Server Error\n",
		date
	);

    // Envoi de la réponse HTTP au client
	int n = send(sockfd, buf, strlen(buf), 0);
    if (n < 0) {
        perror("send");
    }

    // Libération de la mémoire
    free(date);
}

/*
 * Function:  get_http_date
 * --------------------
 * Get datetime in HTTP format
 *
 *  returns: char* datetime
 */
char* get_http_date() {
    // Récupération du timestamp actuel
	time_t now = time(0);

    // Allocation de la variable utilisée pour stocker la date
	char* date = malloc(30 * sizeof(char));

    // Conversion de la date vers un format GMT (cf. rfc2616 - section-3.3.1)
	struct tm tm = *gmtime(&now);

    // Conversion de la date dans le format recommandé pour HTTP
	strftime(date, 30, "%a, %d %b %Y %H:%M:%S %Z", &tm);

	return date;
}

/*
 * Function:  get_int_length
 * --------------------
 * Get number of character in a integer
 *
 *  int value : integer whose value we are looking for
 *
 *  returns: int number of character in the integer
 */
int get_int_length(int value) {
	if (value < 10) return 1;
	return 1 + get_int_length(value / 10);
}

/*
 * Function:  fsize
 * --------------------
 * Get the size of a file
 *
 *  FILE* file : the file stream
 *
 *  returns: long length in bytes of the file
 */
long fsize(FILE* file) {
    // On déplace le curseur à la fin du fichier
	fseek(file, 0L, SEEK_END);

    // On récupère la position du curseur
	long size = ftell(file);

    // On remet le curseur au début
	fseek(file, 0L, SEEK_SET);

	return size;
}
