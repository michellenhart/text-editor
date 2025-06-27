// editor_colaborativo.c
// Versão com propagação de edições e simulação automática com OpenMP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mpi.h>
#include <omp.h>

#define MAX_LINHAS 100
#define MAX_TAM_LINHA 256
#define TAG_EDITAR 1
#define TAG_LIBERAR 2
#define TAG_MSG_PRIVADA 3
#define TAG_RESPOSTA 4
#define TAG_CONTEUDO 5
#define TAG_SAIR 6

char documento[MAX_LINHAS][MAX_TAM_LINHA];
int bloqueada[MAX_LINHAS];

void coordenador(int numprocs) {
    MPI_Status status;
    int linha;
    char buffer[MAX_TAM_LINHA];

    for (int i = 0; i < MAX_LINHAS; i++) {
        sprintf(documento[i], "Linha %d vazia", i);
        bloqueada[i] = -1;
    }

    while (1) {
        MPI_Recv(buffer, MAX_TAM_LINHA, MPI_CHAR, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        int remetente = status.MPI_SOURCE;

        if (status.MPI_TAG == TAG_EDITAR) {
            sscanf(buffer, "%d", &linha);
            if (bloqueada[linha] == -1) {
                bloqueada[linha] = remetente;
                sprintf(buffer, "OK - Linha %d liberada para voce", linha);
            } else {
                sprintf(buffer, "NEGADO - Linha %d esta sendo editada por %d", linha, bloqueada[linha]);
            }
            MPI_Send(buffer, strlen(buffer)+1, MPI_CHAR, remetente, TAG_RESPOSTA, MPI_COMM_WORLD);
        }
        else if (status.MPI_TAG == TAG_LIBERAR) {
            sscanf(buffer, "%d", &linha);
            if (bloqueada[linha] == remetente) {
                bloqueada[linha] = -1;
                sprintf(buffer, "Linha %d liberada", linha);
                MPI_Send(buffer, strlen(buffer)+1, MPI_CHAR, remetente, TAG_RESPOSTA, MPI_COMM_WORLD);
            }
        }
        else if (status.MPI_TAG == TAG_MSG_PRIVADA) {
            int destino;
            char msg[MAX_TAM_LINHA];
            sscanf(buffer, "%d %[^"]", &destino, msg);
            MPI_Send(msg, strlen(msg)+1, MPI_CHAR, destino, TAG_MSG_PRIVADA, MPI_COMM_WORLD);
        }
        else if (status.MPI_TAG == TAG_CONTEUDO) {
            sscanf(buffer, "%d %[^"]", &linha, documento[linha]);
            // Propaga a edição para todos os outros
            for (int i = 1; i < numprocs; i++) {
                if (i != remetente) {
                    MPI_Send(buffer, strlen(buffer)+1, MPI_CHAR, i, TAG_CONTEUDO, MPI_COMM_WORLD);
                }
            }
        }
    }
}

void editor_usuario(int rank) {
    char buffer[MAX_TAM_LINHA];
    int linha;
    MPI_Status status;

    #pragma omp parallel sections
    {
        #pragma omp section
        {
            while (1) {
                printf("[Editor %d] Digite linha para editar (-1 para sair): ", rank);
                scanf("%d", &linha);
                if (linha == -1) break;

                sprintf(buffer, "%d", linha);
                MPI_Send(buffer, strlen(buffer)+1, MPI_CHAR, 0, TAG_EDITAR, MPI_COMM_WORLD);

                MPI_Recv(buffer, MAX_TAM_LINHA, MPI_CHAR, 0, TAG_RESPOSTA, MPI_COMM_WORLD, &status);
                printf("[Editor %d] Resposta: %s\n", rank, buffer);

                if (strstr(buffer, "OK")) {
                    printf("[Editor %d] Conteudo atual: %s\n", rank, documento[linha]);
                    printf("[Editor %d] Digite novo conteudo: ", rank);
                    scanf(" %[^"]", buffer);

                    char msg[MAX_TAM_LINHA];
                    sprintf(msg, "%d %s", linha, buffer);
                    MPI_Send(msg, strlen(msg)+1, MPI_CHAR, 0, TAG_CONTEUDO, MPI_COMM_WORLD);

                    sprintf(buffer, "%d", linha);
                    MPI_Send(buffer, strlen(buffer)+1, MPI_CHAR, 0, TAG_LIBERAR, MPI_COMM_WORLD);
                }
            }
        }

        #pragma omp section
        {
            while (1) {
                MPI_Recv(buffer, MAX_TAM_LINHA, MPI_CHAR, 0, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
                if (status.MPI_TAG == TAG_CONTEUDO) {
                    int linha;
                    char conteudo[MAX_TAM_LINHA];
                    sscanf(buffer, "%d %[^"]", &linha, conteudo);
                    strncpy(documento[linha], conteudo, MAX_TAM_LINHA);
                    printf("[Editor %d] Linha %d atualizada: %s\n", rank, linha, documento[linha]);
                }
                else if (status.MPI_TAG == TAG_MSG_PRIVADA) {
                    printf("[Editor %d] MSG PRIVADA: %s\n", rank, buffer);
                }
            }
        }
    }
}

void simular_edicoes(int rank, int num_iteracoes) {
    #pragma omp parallel for
    for (int i = 0; i < num_iteracoes; i++) {
        int linha = rand() % MAX_LINHAS;
        char buffer[MAX_TAM_LINHA];

        sprintf(buffer, "%d", linha);
        MPI_Send(buffer, strlen(buffer)+1, MPI_CHAR, 0, TAG_EDITAR, MPI_COMM_WORLD);
        MPI_Recv(buffer, MAX_TAM_LINHA, MPI_CHAR, 0, TAG_RESPOSTA, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        if (strstr(buffer, "OK")) {
            char msg[MAX_TAM_LINHA];
            sprintf(msg, "%d Simulacao linha %d por %d", linha, linha, rank);
            MPI_Send(msg, strlen(msg)+1, MPI_CHAR, 0, TAG_CONTEUDO, MPI_COMM_WORLD);

            sprintf(buffer, "%d", linha);
            MPI_Send(buffer, strlen(buffer)+1, MPI_CHAR, 0, TAG_LIBERAR, MPI_COMM_WORLD);
        }
    }
}

int main(int argc, char** argv) {
    int rank, size;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (rank == 0) {
        coordenador(size);
    } else {
        if (argc > 1 && strcmp(argv[1], "--simular") == 0) {
            simular_edicoes(rank, 50); // Simula 50 edicoes por processo
        } else {
            editor_usuario(rank);
        }
    }

    MPI_Finalize();
    return 0;
}