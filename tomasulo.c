#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_REG 32
#define MAX_RS  5
#define MAX_INS 200
#define MAX_MEM 1024

typedef enum { ADD, SUB, MUL, DIV, LD, ST, NOP, INVALID } OpCode;

typedef struct {
    OpCode op;
    int rd, rs1, rs2;   // rd (dest), rs1 (base p/ LD/ST), rs2 (src2)
    int imm;            // offset p/ LD/ST
    char raw[128];
} Instruction;

typedef struct {
    int busy;
    OpCode op;
    int Vj, Vk;         // valores dos operandos
    int Qj, Qk;         // tags pendentes (0 = pronto)
    int dest;           // registrador destino (para LD/ALU)
    int remaining;      // ciclos restantes
    int result;         // resultado pronto para WB
    int address;        // endereço efetivo (LD/ST)
    int isStore;        // 1 = store
    int issued;         // marcador opcional
    int exec_started;   // começou a executar?
    char name[4];       // "A1", "M2", "L1", ...
} ReservationStation;

typedef struct {
    int value;
    int Qi;             // tag da RS que vai escrever
} Register;

/* --------- Estado global --------- */
Instruction instructions[MAX_INS];
int num_instructions = 0;

ReservationStation addRS[MAX_RS];
ReservationStation mulRS[MAX_RS];
ReservationStation ldRS[MAX_RS];
ReservationStation stRS[MAX_RS];
Register regs[MAX_REG];
int memv[MAX_MEM];

int cycle = 0;
int pc = 0;

/* Latências padrão (pode ajustar se quiser) */
int LAT_ADD = 2;
int LAT_MUL = 10;
int LAT_LD  = 2;
int LAT_ST  = 2;

/* --------- Utilitários --------- */
static const char* op_to_str(OpCode op) {
    switch(op){
        case ADD: return "ADD";
        case SUB: return "SUB";
        case MUL: return "MUL";
        case DIV: return "DIV";
        case LD:  return "LD";
        case ST:  return "ST";
        case NOP: return "NOP";
        default:  return "-";
    }
}
OpCode parseOp(char *s) {
    for (char *p = s; *p; ++p) *p = (char)toupper(*p);
    if (!strcmp(s, "ADD")) return ADD;
    if (!strcmp(s, "SUB")) return SUB;
    if (!strcmp(s, "MUL")) return MUL;
    if (!strcmp(s, "DIV")) return DIV;
    if (!strcmp(s, "LD"))  return LD;
    if (!strcmp(s, "ST"))  return ST;
    if (!strcmp(s, "NOP")) return NOP;
    return INVALID;
}

void reset_rs(ReservationStation *r) {
    char keep[4] = {0};
    if (r->name[0]) strcpy(keep, r->name);
    r->busy = 0;
    r->op = NOP;
    r->Vj = r->Vk = 0;
    r->Qj = r->Qk = 0;
    r->dest = -1;
    r->remaining = 0;
    r->result = 0;
    r->address = 0;
    r->isStore = 0;
    r->issued = 0;
    r->exec_started = 0;
    if (keep[0]) strcpy(r->name, keep);
}

int findFreeRS(ReservationStation *rs, int n) {
    for (int i = 0; i < n; i++)
        if (!rs[i].busy) return i;
    return -1;
}

/* --------- I/O de Programa --------- */
void loadInstructions(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Erro ao abrir %s\n", filename);
        exit(1);
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        // ignora comentários/linhas vazias
        int allspace = 1;
        for (int i=0; line[i]; ++i) if (!isspace((unsigned char)line[i])) { allspace = 0; break; }
        if (line[0]=='#' || allspace) continue;

        Instruction inst; memset(&inst, 0, sizeof(inst));
        strncpy(inst.raw, line, sizeof(inst.raw)-1);

        char op[16] = {0};
        sscanf(line, "%15s", op);
        inst.op = parseOp(op);

        switch (inst.op) {
            case ADD: case SUB: case MUL: case DIV:
                // ADD rd, rs1, rs2
                // espaços e vírgulas liberais
                sscanf(line, "%*s R%d , R%d , R%d", &inst.rd, &inst.rs1, &inst.rs2);
                break;
            case LD:
                // LD rd, imm(Rrs1)
                sscanf(line, "%*s R%d , %d(R%d)", &inst.rd, &inst.imm, &inst.rs1);
                break;
            case ST:
                // ST Rrs2 , imm(Rrs1)   (valor em rs2 -> memória[imm + Rrs1])
                sscanf(line, "%*s R%d , %d(R%d)", &inst.rs2, &inst.imm, &inst.rs1);
                break;
            case NOP:
                break;
            default:
                fprintf(stderr, "Linha inválida: %s", line);
                continue;
        }
        instructions[num_instructions++] = inst;
        if (num_instructions >= MAX_INS) break;
    }
    fclose(f);
}

/* --------- Impressão organizada --------- */
void printRS(ReservationStation *rs, int n, const char *title) {
    printf("\n--- %s ---\n", title);
    printf("%-3s | %-4s | %-4s | %-6s | %-6s | %-4s | %-4s | %-4s | %-4s | %-5s | %-6s\n",
           "ID", "Busy", "Op", "Vj", "Vk", "Qj", "Qk", "Dest", "Rem", "Addr", "Res");
    printf("---------------------------------------------------------------------------------------\n");
    for (int i = 0; i < n; i++) {
        if (rs[i].busy) {
            printf("%-3s | %-4d | %-4s | %-6d | %-6d | %-4d | %-4d | %-4d | %-4d | %-5d | %-6d\n",
                   rs[i].name, rs[i].busy, op_to_str(rs[i].op),
                   rs[i].Vj, rs[i].Vk, rs[i].Qj, rs[i].Qk, rs[i].dest,
                   rs[i].remaining, rs[i].isStore ? rs[i].address : rs[i].address,
                   rs[i].result);
        } else {
            printf("%-3s | %-4d | %-4s | %-6s | %-6s | %-4s | %-4s | %-4s | %-4s | %-5s | %-6s\n",
                   rs[i].name, 0, "-", "-", "-", "-", "-", "-", "-", "-", "-");
        }
    }
}

void printRegs() {
    printf("\n--- REGISTRADORES ---\n");
    for (int i = 0; i < MAX_REG; i++) {
        printf("R%-2d:%6d(Q%-3d)  ", i, regs[i].value, regs[i].Qi);
        if ((i + 1) % 4 == 0) printf("\n");
    }
}

/* --------- Issue / Execute / CDB --------- */
void issue() {
    if (pc >= num_instructions) return;

    Instruction inst = instructions[pc];
    int idx = -1;

    switch (inst.op) {
        case ADD:
        case SUB: {
            idx = findFreeRS(addRS, MAX_RS);
            if (idx == -1) return;
            reset_rs(&addRS[idx]);
            addRS[idx].busy = 1;
            addRS[idx].op = inst.op;
            addRS[idx].dest = inst.rd;
            addRS[idx].remaining = LAT_ADD;
            addRS[idx].Vj = regs[inst.rs1].value;
            addRS[idx].Vk = regs[inst.rs2].value;
            addRS[idx].Qj = regs[inst.rs1].Qi;
            addRS[idx].Qk = regs[inst.rs2].Qi;
            regs[inst.rd].Qi = (idx + 1);          // tags ADD: 1..MAX_RS
            printf("Issue: %s", inst.raw);
            pc++;
            break;
        }
        case MUL:
        case DIV: {
            idx = findFreeRS(mulRS, MAX_RS);
            if (idx == -1) return;
            reset_rs(&mulRS[idx]);
            mulRS[idx].busy = 1;
            mulRS[idx].op = inst.op;
            mulRS[idx].dest = inst.rd;
            mulRS[idx].remaining = LAT_MUL;
            mulRS[idx].Vj = regs[inst.rs1].value;
            mulRS[idx].Vk = regs[inst.rs2].value;
            mulRS[idx].Qj = regs[inst.rs1].Qi;
            mulRS[idx].Qk = regs[inst.rs2].Qi;
            regs[inst.rd].Qi = 100 + (idx + 1);    // tags MUL: 101..100+MAX_RS
            printf("Issue: %s", inst.raw);
            pc++;
            break;
        }
        case LD: {
            idx = findFreeRS(ldRS, MAX_RS);
            if (idx == -1) return;
            reset_rs(&ldRS[idx]);
            ldRS[idx].busy = 1;
            ldRS[idx].op = inst.op;
            ldRS[idx].dest = inst.rd;
            ldRS[idx].remaining = LAT_LD;
            // endereço efetivo simplificado (sem dependência do base)
            ldRS[idx].address = regs[inst.rs1].value + inst.imm;
            regs[inst.rd].Qi = 200 + (idx + 1);    // tags LD: 201..200+MAX_RS
            printf("Issue: %s", inst.raw);
            pc++;
            break;
        }
        case ST: {
            idx = findFreeRS(stRS, MAX_RS);
            if (idx == -1) return;
            reset_rs(&stRS[idx]);
            stRS[idx].busy = 1;
            stRS[idx].op = inst.op;
            stRS[idx].isStore = 1;
            stRS[idx].remaining = LAT_ST;
            stRS[idx].address = regs[inst.rs1].value + inst.imm; // base+offset
            stRS[idx].Vj = regs[inst.rs2].value;                 // valor a armazenar
            stRS[idx].Qj = regs[inst.rs2].Qi;                    // dependência de valor
            printf("Issue: %s", inst.raw);
            pc++;
            break;
        }
        case NOP:
            printf("NOP emitida.\n");
            pc++;
            break;
        default: break;
    }
}

void execute_group(ReservationStation *rs, int n) {
    for (int i = 0; i < n; i++) {
        if (!rs[i].busy || rs[i].remaining <= 0) continue;

        // Só pode começar quando operandos prontos
        int operands_ready = (rs[i].Qj == 0 && rs[i].Qk == 0);
        // Para LD/ST simplificado: LD não usa Qj/Qk; ST depende só de Qj (valor)
        if (rs[i].op == LD) operands_ready = 1; // endereço já resolvido neste modelo
        if (rs[i].op == ST) operands_ready = (rs[i].Qj == 0); // valor pronto

        if (!rs[i].exec_started && operands_ready) {
            rs[i].exec_started = 1;
        }
        if (!rs[i].exec_started) continue;

        rs[i].remaining--;
        if (rs[i].remaining == 0) {
            switch (rs[i].op) {
                case ADD: rs[i].result = rs[i].Vj + rs[i].Vk; break;
                case SUB: rs[i].result = rs[i].Vj - rs[i].Vk; break;
                case MUL: rs[i].result = rs[i].Vj * rs[i].Vk; break;
                case DIV: rs[i].result = (rs[i].Vk ? rs[i].Vj / rs[i].Vk : 0); break;
                case LD:  rs[i].result = memv[rs[i].address % MAX_MEM]; break;
                case ST:  rs[i].result = rs[i].Vj; break; // valor a ser gravado
                default: break;
            }
        }
    }
}

/* Um único CDB por ciclo — prioridade: LD -> ADD -> MUL -> ST */
typedef struct {
    ReservationStation *arr;
    int n;
    int baseTag;
    int isStoreGroup;
} RSGroup;

static int pick_ready_index(RSGroup g) {
    for (int i = 0; i < g.n; i++) {
        if (g.arr[i].busy && g.arr[i].remaining == 0) return i;
    }
    return -1;
}

void wake_deps_with_result(int tag, int value) {
    for (int j = 0; j < MAX_RS; j++) {
        if (addRS[j].busy) {
            if (addRS[j].Qj == tag) { addRS[j].Vj = value; addRS[j].Qj = 0; }
            if (addRS[j].Qk == tag) { addRS[j].Vk = value; addRS[j].Qk = 0; }
        }
        if (mulRS[j].busy) {
            if (mulRS[j].Qj == tag) { mulRS[j].Vj = value; mulRS[j].Qj = 0; }
            if (mulRS[j].Qk == tag) { mulRS[j].Vk = value; mulRS[j].Qk = 0; }
        }
        if (stRS[j].busy) {
            if (stRS[j].Qj == tag) { stRS[j].Vj = value; stRS[j].Qj = 0; }
        }
        // ldRS não depende de operandos neste modelo
    }
}

void writeBack_oneCDB() {
    RSGroup groups[4] = {
        { ldRS,  MAX_RS, 200, 0 },
        { addRS, MAX_RS,   0, 0 },
        { mulRS, MAX_RS, 100, 0 },
        { stRS,  MAX_RS, 300, 1 }
    };

    int gi = -1, si = -1;
    for (int g = 0; g < 4; g++) {
        si = pick_ready_index(groups[g]);
        if (si >= 0) { gi = g; break; }
    }
    if (gi < 0) return; // ninguém pronto pra WB neste ciclo

    ReservationStation *r = &groups[gi].arr[si];
    int tag = groups[gi].baseTag + si + 1;

    if (groups[gi].isStoreGroup || r->isStore) {
        // Commit do store
        memv[r->address % MAX_MEM] = r->result;
        printf("WB: Store %d -> Mem[%d]\n", r->result, r->address % MAX_MEM);
        reset_rs(r);
        return;
    }

    // Broadcast para registradores
    for (int rr = 0; rr < MAX_REG; rr++) {
        if (regs[rr].Qi == tag) {
            regs[rr].value = r->result;
            regs[rr].Qi = 0;
        }
    }
    // Acorda dependentes
    wake_deps_with_result(tag, r->result);

    printf("WB: resultado %d escrito (tag %d)\n", r->result, tag);
    reset_rs(r);
}

/* --------- Step / Done / Main --------- */
void step() {
    cycle++;
    printf("\n===== Ciclo %d =====\n", cycle);

    issue();

    execute_group(addRS, MAX_RS);
    execute_group(mulRS, MAX_RS);
    execute_group(ldRS, MAX_RS);
    execute_group(stRS, MAX_RS);

    writeBack_oneCDB();

    printRS(addRS, MAX_RS, "ADD/SUB Stations");
    printRS(mulRS, MAX_RS, "MUL/DIV Stations");
    printRS(ldRS, MAX_RS,  "LOAD Buffers");
    printRS(stRS, MAX_RS,  "STORE Buffers");
    printRegs();
}

int done() {
    if (pc < num_instructions) return 0;
    for (int i = 0; i < MAX_RS; i++)
        if (addRS[i].busy || mulRS[i].busy || ldRS[i].busy || stRS[i].busy)
            return 0;
    return 1;
}

int main() {
    // Nomeia estações e reseta
    for (int i = 0; i < MAX_RS; i++) {
        sprintf(addRS[i].name, "A%d", i + 1);
        sprintf(mulRS[i].name, "M%d", i + 1);
        sprintf(ldRS[i].name,  "L%d", i + 1);
        sprintf(stRS[i].name,  "S%d", i + 1);
        reset_rs(&addRS[i]);
        reset_rs(&mulRS[i]);
        reset_rs(&ldRS[i]);
        reset_rs(&stRS[i]);
    }

    // Inicializa registradores e memória com valores distintos
    for (int i = 0; i < MAX_REG; i++) { regs[i].value = i * 10; regs[i].Qi = 0; }
    for (int i = 0; i < MAX_MEM; i++) memv[i] = i + 1000;

    loadInstructions("instructions.txt");

    // Simulação
    while (!done() && cycle < 500) step();

    printf("\nExecução concluída em %d ciclos.\n", cycle);
    return 0;
}
