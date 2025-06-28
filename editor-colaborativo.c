/*
Instalar biblioteca:
	sudo apt-get install libgtk-3-dev
Compilar:
    mpicc editor-colaborativo.c -o editor-colaborativo -fopenmp pkg-config --cflags --libs gtk+-3.0
Executar:
    mpirun -np 2 ./editor-colaborativo
    (1 servidor - rank 0, 1 cliente - rank 1)
*/

#include <mpi.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <omp.h>

#define MAX_LINE_LENGTH 256
#define MAX_LINES 10
#define MSG_LENGTH 256

typedef enum {
    MSG_TEXT_FULL,
    MSG_LINE_RESERVE,
    MSG_LINE_GRANTED,
    MSG_LINE_DENIED,
    MSG_LINE_EDIT,
    MSG_LINE_RELEASE,
    MSG_CHAT_PRIVATE,
    MSG_GEN_DATA
} MSG_TYPE;

typedef struct {
    MSG_TYPE type;
    int sender_rank;
    int target_rank; // Para mensagens privadas
    int line_no;
    char text[MAX_LINE_LENGTH];
} Message;

// =================== SERVIDOR (rank 0) =====================

typedef struct {
    char text[MAX_LINE_LENGTH];
    int locked_by; // -1 = livre, >=0 = id do usuário
} Line;

Line document[MAX_LINES];

void broadcast_text(int comm_size) {
    Message msg;
    msg.type = MSG_TEXT_FULL;
    msg.sender_rank = 0;
    for (int i = 0; i < MAX_LINES; i++) {
        strcpy(msg.text, document[i].text);
        msg.line_no = i;
        for (int dest = 1; dest < comm_size; dest++) {
            MPI_Send(&msg, sizeof(msg), MPI_BYTE, dest, 0, MPI_COMM_WORLD);
        }
    }
}

void handle_server(int comm_size) {
    Message msg, reply;
    MPI_Status status;

    // Inicializa documento vazio
    for (int i = 0; i < MAX_LINES; i++) {
        document[i].locked_by = -1;
        document[i].text[0] = '\0'; // Começa vazio
    }

    printf("Servidor iniciado. Esperando clientes...\n");
    broadcast_text(comm_size);

    while (1) {
        MPI_Recv(&msg, sizeof(msg), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

        switch (msg.type) {
            case MSG_LINE_RESERVE:
                if (document[msg.line_no].locked_by == -1) {
                    document[msg.line_no].locked_by = msg.sender_rank;
                    reply.type = MSG_LINE_GRANTED;
					reply.sender_rank = 0;
                    // Log
                    printf("[LOG] Usuario %d reservou a linha %d\n", msg.sender_rank, msg.line_no + 1);
                } else {
                    reply.type = MSG_LINE_DENIED;
					reply.sender_rank = document[msg.line_no].locked_by; // quem está editando
                    // Log
                    printf("[LOG] Usuario %d tentou reservar a linha %d, mas ela está ocupada por Usuario %d\n", msg.sender_rank, msg.line_no + 1, document[msg.line_no].locked_by);
                }
                reply.line_no = msg.line_no;
                //reply.sender_rank = 0;
                MPI_Send(&reply, sizeof(reply), MPI_BYTE, msg.sender_rank, 0, MPI_COMM_WORLD);
                break;

            case MSG_LINE_EDIT:
                if (document[msg.line_no].locked_by == msg.sender_rank) {
                    #pragma omp critical
                    {
                        strncpy(document[msg.line_no].text, msg.text, MAX_LINE_LENGTH);
                    }
                    // Log
                    printf("[LOG] Usuario %d editou linha %d: \"%s\"\n", msg.sender_rank, msg.line_no + 1, msg.text);
    
                    // Broadcast nova linha a todos, inclusive quem editou!
                    reply = msg;
                    for (int dest = 1; dest < comm_size; dest++) {
                        MPI_Send(&reply, sizeof(reply), MPI_BYTE, dest, 0, MPI_COMM_WORLD);
                    }
                } else {
                    // Log
                    printf("[LOG] Usuario %d tentou editar linha %d sem possuir o bloqueio\n", msg.sender_rank, msg.line_no + 1);
                }
                break;

            case MSG_LINE_RELEASE:
                if (document[msg.line_no].locked_by == msg.sender_rank) {
                    document[msg.line_no].locked_by = -1;
                    // Log
                    printf("[LOG] Usuario %d liberou a linha %d\n", msg.sender_rank, msg.line_no + 1);
                } else {
                    // Log
                    printf("[LOG] Usuario %d tentou liberar linha %d sem possuir o bloqueio\n", msg.sender_rank, msg.line_no + 1);
                }
                break;


            case MSG_CHAT_PRIVATE:
                // Log
                printf("[LOG] Usuario %d enviou mensagem privada para Usuario %d: \"%s\"\n", msg.sender_rank, msg.target_rank, msg.text);
                MPI_Send(&msg, sizeof(msg), MPI_BYTE, msg.target_rank, 0, MPI_COMM_WORLD);
                break;

            case MSG_GEN_DATA:
                // Log
                printf("[LOG] Usuario %d solicitou geração automática de dados.\n", msg.sender_rank);
                #pragma omp parallel for
                for (int i = 0; i < MAX_LINES; i++)
                    snprintf(document[i].text, MAX_LINE_LENGTH, "Linha %d: Dado gerado automaticamente", i+1);
                broadcast_text(comm_size);
                break;

            default:
                break;
        }
    }
}

// ============== CLIENTE (ranks != 0) + GTK ==============

typedef struct {
    int my_rank;
    int world_size;
    char document[MAX_LINES][MAX_LINE_LENGTH];
    int editing_line;
    GtkWidget *window;
    GtkWidget *text_view;
    GtkWidget *entry_msg;
    GtkWidget *label_status;
    GtkTextBuffer *buffer;
    char status_buf[256];
    char msg_buf[MSG_LENGTH];
    int waiting_line_response;
    int pending_line_no;
    char pending_line_text[MAX_LINE_LENGTH];
	char username[16];
	GtkWidget *combo_users;
} ClientState;

// Atualiza o textview com todas as linhas não vazias
void update_text_view(ClientState *state) {
    char all_text[MAX_LINES * MAX_LINE_LENGTH] = "";
    for (int i = 0; i < MAX_LINES; i++) {
        if (state->document[i][0] != '\0') {
            strcat(all_text, state->document[i]);
        }
        strcat(all_text, "\n");
    }
    gtk_text_buffer_set_text(state->buffer, all_text, -1);
}

gboolean gui_update_text_view(gpointer user_data) {
    update_text_view((ClientState*)user_data);
    return FALSE;
}

gboolean gui_set_status(gpointer user_data) {
    ClientState *state = (ClientState*)user_data;
    gtk_label_set_text(GTK_LABEL(state->label_status), state->status_buf);
    return FALSE;
}

// Função para abrir dialog para digitar o texto da linha
gboolean open_edit_text_dialog(gpointer user_data) {
    ClientState *state = (ClientState*)user_data;
    int line_no = state->editing_line;

    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Editar Linha",
        GTK_WINDOW(state->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_OK", GTK_RESPONSE_ACCEPT,
        "_Cancelar", GTK_RESPONSE_REJECT,
        NULL
    );
	
	gtk_window_set_default_size(GTK_WINDOW(dialog), 600, 75);
	
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *entry = gtk_entry_new();

    // Preenche com o texto atual da linha (se houver)
    gtk_entry_set_text(GTK_ENTRY(entry), state->document[line_no]);

    gtk_box_pack_start(GTK_BOX(content_area), entry, TRUE, TRUE, 0);
    gtk_widget_show_all(dialog);

    gint result = gtk_dialog_run(GTK_DIALOG(dialog));
    if (result == GTK_RESPONSE_ACCEPT) {
        const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
        Message msg;
        msg.type = MSG_LINE_EDIT;
        msg.sender_rank = state->my_rank;
        msg.line_no = line_no;
        snprintf(msg.text, MAX_LINE_LENGTH, "%s", text);
        MPI_Send(&msg, sizeof(msg), MPI_BYTE, 0, 0, MPI_COMM_WORLD);

        msg.type = MSG_LINE_RELEASE;
        MPI_Send(&msg, sizeof(msg), MPI_BYTE, 0, 0, MPI_COMM_WORLD);
        state->editing_line = -1;
        gtk_label_set_text(GTK_LABEL(state->label_status), "Linha editada e liberada.");
    } else {
        // Se cancelar, libera a linha também
        Message msg;
        msg.type = MSG_LINE_RELEASE;
        msg.sender_rank = state->my_rank;
        msg.line_no = line_no;
        MPI_Send(&msg, sizeof(msg), MPI_BYTE, 0, 0, MPI_COMM_WORLD);
        state->editing_line = -1;
        gtk_label_set_text(GTK_LABEL(state->label_status), "Edição cancelada, linha liberada.");
    }
    gtk_widget_destroy(dialog);
    return FALSE;
}

// Função para abrir dialog para digitar o número da linha
void on_edit_clicked(GtkButton *button, gpointer user_data) {
    ClientState *state = (ClientState*)user_data;
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Reservar Linha",
        GTK_WINDOW(state->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_OK", GTK_RESPONSE_ACCEPT,
        "_Cancelar", GTK_RESPONSE_REJECT,
        NULL
    );
	
	gtk_window_set_default_size(GTK_WINDOW(dialog), 250, 75);
	
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Número da linha (1 a 10)");
    gtk_box_pack_start(GTK_BOX(content_area), entry, TRUE, TRUE, 0);
    gtk_widget_show_all(dialog);

    gint result = gtk_dialog_run(GTK_DIALOG(dialog));
    if (result == GTK_RESPONSE_ACCEPT) {
        const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
        int line = atoi(text);
        if (line < 1 || line > MAX_LINES) {
            snprintf(state->status_buf, sizeof(state->status_buf), "Linha inválida.");
            g_idle_add(gui_set_status, state);
        } else {
            // Solicita reserva da linha ao servidor
            Message msg;
            msg.type = MSG_LINE_RESERVE;
            msg.sender_rank = state->my_rank;
            msg.line_no = line-1;
            state->waiting_line_response = 1;
            state->pending_line_no = line-1;
            MPI_Send(&msg, sizeof(msg), MPI_BYTE, 0, 0, MPI_COMM_WORLD);
        }
    }
    gtk_widget_destroy(dialog);
}

// Listener MPI/GTK
void run_listener(ClientState *state) {
    Message msg;
    MPI_Status status;
    while (1) {
        int flag = 0;
        // Serve para consultar se há uma mensagem chegando para o destino.
        // flag retorna 1 se há.
        MPI_Iprobe(0, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);
        if (!flag) {
            g_usleep(5000); // 5 ms
            continue;            
        }

        MPI_Recv(&msg, sizeof(msg), MPI_BYTE, 0, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        switch (msg.type) {
            case MSG_TEXT_FULL:
                strncpy(state->document[msg.line_no], msg.text, MAX_LINE_LENGTH);
                g_idle_add(gui_update_text_view, state);
                break;
            case MSG_LINE_EDIT:
                strncpy(state->document[msg.line_no], msg.text, MAX_LINE_LENGTH);
                g_idle_add(gui_update_text_view, state);
                break;
            case MSG_LINE_GRANTED:
                state->editing_line = msg.line_no;
                snprintf(state->status_buf, sizeof(state->status_buf), "%s reservou a linha %d.", state->username, state->editing_line+1);
                g_idle_add(gui_set_status, state);
                // Somente se foi solicitação do usuário atual
                if (state->waiting_line_response && state->pending_line_no == msg.line_no) {
                    state->waiting_line_response = 0;
                    // Chama dialog para editar texto da linha
                    g_idle_add(open_edit_text_dialog, state);
                }
                break;
            case MSG_LINE_DENIED:
                snprintf(state->status_buf, sizeof(state->status_buf), "Linha %d está sendo editada por outro usuário.", msg.line_no+1);
                char locking_user[32];
                snprintf(locking_user, sizeof(locking_user), "Usuario%d", msg.sender_rank);
                snprintf(state->status_buf, sizeof(state->status_buf), "Linha %d está sendo editada por %s.", msg.line_no+1, locking_user);
                g_idle_add(gui_set_status, state);
                state->waiting_line_response = 0;
                break;
            case MSG_CHAT_PRIVATE:
                char sender_name[32];
                snprintf(sender_name, sizeof(sender_name), "Usuario%d", msg.sender_rank);
                int prefix_len = snprintf(state->status_buf, sizeof(state->status_buf), "[%s → Você] ", sender_name);
                int max_text_len = sizeof(state->status_buf) - prefix_len - 1;
                strncat(state->status_buf, msg.text, max_text_len);
                g_idle_add(gui_set_status, state);
                break;
            default:
                break;
        }
    }
}

void on_private_msg(GtkButton *button, gpointer user_data) {
    ClientState *state = (ClientState*)user_data;

	// Verifica se ha mais de um usuario (alem do proprio)
    if (state->world_size <= 2) { // 1 servidor + 1 cliente = so o proprio
        gtk_label_set_text(GTK_LABEL(state->label_status), "Não há outros usuários conectados para enviar mensagem.");
        return;
    }
	
	const char *target_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(state->combo_users));
	
	if (!target_id || strcmp(target_id, "none") == 0) {
		gtk_label_set_text(GTK_LABEL(state->label_status), "Selecione um usuário destino.");
		return;
	}

	int target_rank = atoi(target_id);
	if (target_rank == state->my_rank || target_rank <= 0 || target_rank >= state->world_size) {
		gtk_label_set_text(GTK_LABEL(state->label_status), "Usuário destino inválido.");
		return;
	}

	const char *msg_text = gtk_entry_get_text(GTK_ENTRY(state->entry_msg));
	if (msg_text == NULL || strlen(msg_text) == 0) {
		gtk_label_set_text(GTK_LABEL(state->label_status), "Digite uma mensagem antes de enviar.");
		return;
	}

    Message msg;
    msg.type = MSG_CHAT_PRIVATE;
    msg.sender_rank = state->my_rank;
    msg.target_rank = target_rank;
    snprintf(msg.text, MSG_LENGTH, "%s", gtk_entry_get_text(GTK_ENTRY(state->entry_msg)));

	MPI_Send(&msg, sizeof(msg), MPI_BYTE, 0, 0, MPI_COMM_WORLD);
    gtk_label_set_text(GTK_LABEL(state->label_status), "Mensagem privada enviada.");
	gtk_entry_set_text(GTK_ENTRY(state->entry_msg), "");
}

void on_gen_data(GtkButton *button, gpointer user_data) {
    ClientState *state = (ClientState*)user_data;
    Message msg;
    msg.type = MSG_GEN_DATA;
    msg.sender_rank = state->my_rank;
    MPI_Send(&msg, sizeof(msg), MPI_BYTE, 0, 0, MPI_COMM_WORLD);
    gtk_label_set_text(GTK_LABEL(state->label_status), "Solicitado preenchimento automático.");
}

void setup_gui(ClientState *state, int argc, char **argv) {
    gtk_init(&argc, &argv);

    state->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	char window_title[64];
	snprintf(window_title, sizeof(window_title), "Editor Colaborativo - %s", state->username);
    gtk_window_set_title(GTK_WINDOW(state->window), window_title);
    gtk_window_set_default_size(GTK_WINDOW(state->window), 300, 200);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(state->window), vbox);

    // Usa scrolledwindow para limitar tamanho do textview
    state->text_view = gtk_text_view_new();
    state->buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->text_view));
    gtk_text_view_set_editable(GTK_TEXT_VIEW(state->text_view), TRUE);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_size_request(scrolled, 260, 80); // Reduzido
    gtk_container_add(GTK_CONTAINER(scrolled), state->text_view);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled, TRUE, TRUE, 0);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

    GtkWidget *btn_edit = gtk_button_new_with_label("Reservar Linha");
    g_signal_connect(btn_edit, "clicked", G_CALLBACK(on_edit_clicked), state);
    gtk_box_pack_start(GTK_BOX(hbox), btn_edit, FALSE, FALSE, 0);

    //GtkWidget *btn_commit = gtk_button_new_with_label("Confirmar Edição");
    //gtk_widget_set_sensitive(btn_commit, FALSE); // Não mais usado, agora edita via dialog
    //gtk_box_pack_start(GTK_BOX(hbox), btn_commit, FALSE, FALSE, 0);

    state->entry_msg = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(state->entry_msg), "Mensagem privada");
    gtk_box_pack_start(GTK_BOX(hbox), state->entry_msg, TRUE, TRUE, 0);

	state->combo_users = gtk_combo_box_text_new();
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->combo_users), "none", "Selecione...");

	for (int i = 1; i < state->world_size; i++) {
		if (i == state->my_rank) continue;
		char label[32], value[16];
		snprintf(label, sizeof(label), "Usuario%d", i);
		snprintf(value, sizeof(value), "%d", i); // usamos o rank como valor
		gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->combo_users), value, label);
	}
	gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->combo_users), "none");
	gtk_box_pack_start(GTK_BOX(hbox), state->combo_users, FALSE, FALSE, 0);

    GtkWidget *btn_msg = gtk_button_new_with_label("Enviar Privado");
    g_signal_connect(btn_msg, "clicked", G_CALLBACK(on_private_msg), state);
    gtk_box_pack_start(GTK_BOX(hbox), btn_msg, FALSE, FALSE, 0);

    GtkWidget *btn_gen = gtk_button_new_with_label("Gerar Dados de Teste");
    g_signal_connect(btn_gen, "clicked", G_CALLBACK(on_gen_data), state);
    gtk_box_pack_start(GTK_BOX(hbox), btn_gen, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);

    state->label_status = gtk_label_new("Pronto.");
    gtk_widget_set_margin_bottom(state->label_status, 8);
    gtk_box_pack_start(GTK_BOX(vbox), state->label_status, FALSE, FALSE, 0);

    gtk_widget_show_all(state->window);
	
	printf("Conectado como %s (rank %d)\n", state->username, state->my_rank);
	printf("Usuários conectados:\n");
	for (int i = 1; i < state->world_size; i++) {
		printf("  Usuario%d%s\n", i, (i == state->my_rank ? " (você)" : ""));
	}

    g_signal_connect(state->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
}

int main(int argc, char **argv) {
    int rank, size;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (rank == 0) {
        handle_server(size);
        MPI_Finalize();
        return 0;
    }

    // =========== CLIENTE ===========
    ClientState state = {0};
    state.my_rank = rank;
    state.world_size = size;
    state.editing_line = -1;
    state.waiting_line_response = 0;
	snprintf(state.username, sizeof(state.username), "Usuario%d", rank);
    for (int i = 0; i < MAX_LINES; i++)
        state.document[i][0] = '\0';

    setup_gui(&state, argc, argv);

    // Região paralela OpenMP com duas seções: GTK e listener
    #pragma omp parallel sections
    {
        #pragma omp section
        {
            gtk_main();
            exit(0);
        }
        #pragma omp section
        {
            run_listener(&state);
        }
    }

    MPI_Finalize();
    return 0;
}