# Projeto: Simulador de Tomasulo (C)

**Participantes**

* Mateus Viana Maschietto
* Lucas Camarino
* Joao Avila
* Joao Gabriel Dias


## 1) O que é

Este programa simula, de forma didática, um processador com **estações de reserva** (Tomasulo) e **um único barramento de resultados (CDB)**.
Ele emite, executa e faz *write-back* de instruções com latências configuráveis, acompanhando dependências por **tags**.

Suporta: `ADD`, `SUB`, `MUL`, `DIV`, `LD` (load), `ST` (store) e `NOP`.

---

## 2) Como compilar e executar

```bash
# compilar
gcc -o tomasulo tomasulo.c

# o programa lê instruções de "instructions.txt"
./tomasulo
```

> Arquivo de entrada: `instructions.txt` (uma instrução por linha).

---

## 3) Formato das instruções (arquivo `instructions.txt`)

* **ADD/Sub/MUL/DIV**: `OP Rd, Rs1, Rs2`
  Ex.: `ADD R1, R2, R3`
* **Load**: `LD Rd, offset(Rs1)`
  Ex.: `LD R4, 8(R2)`  → `R4 = MEM[R2 + 8]`
* **Store**: `ST Rs2, offset(Rs1)`
  Ex.: `ST R5, 12(R3)` → `MEM[R3 + 12] = R5`
* **NOP**: `NOP`

Comentários iniciados por `#` e linhas em branco são ignorados.

---

## 4) Parâmetros principais (fáceis de alterar no código)

* Tamanhos:

  * `MAX_REG = 32` (registradores)
  * `MAX_RS  = 5`  (estações por grupo: ADD/MUL/LD/ST)
  * `MAX_INS = 200` (nº máx. de instruções)
  * `MAX_MEM = 1024` (tamanho da memória simulada)
* Latências (em ciclos):

  * `LAT_ADD = 2`, `LAT_MUL = 10`, `LAT_LD = 2`, `LAT_ST = 2`

  > Ajuste para testar diferentes cenários.

---

## 5) Estados e estruturas (visão rápida)

* **Instruction**: opcode, registradores, *immediate* e linha original.
* **ReservationStation**: *busy*, opcode, `Vj/Vk` (valores), `Qj/Qk` (tags pendentes), `dest`, `remaining` (ciclos restantes), `result`, `address`, `isStore`, `name` (“A1”, “M2”, …).
* **Register**: `value` + `Qi` (tag da RS que irá escrever).

**Grupos de RS**:

* `addRS[]` → `ADD/SUB`
* `mulRS[]` → `MUL/DIV`
* `ldRS[]`  → `LD`
* `stRS[]`  → `ST`

**Tags por grupo**:

* ADD/SUB: `1..MAX_RS`
* MUL/DIV: `101..100+MAX_RS`
* LD: `201..200+MAX_RS`
* ST: (usadas só p/ controle interno, *write-back* de store vai para memória)

---

## 6) Fluxo por ciclo

A cada `step()` (um ciclo):

1. **Issue**

   * Busca a próxima instrução (`pc`) e tenta alocar uma RS livre do grupo correspondente.
   * Copia `Vj/Vk` se disponíveis; senão preenche `Qj/Qk` com **tags**.
   * Para `LD`: calcula endereço efetivo simples (`regs[base] + offset`).
   * Para `ST`: calcula o endereço e depende apenas do valor a armazenar (`Qj`).
2. **Execute**

   * Uma RS só começa a executar quando seus operandos estão prontos (`Qj == 0` e `Qk == 0`).
   * Decrementa `remaining` a cada ciclo; ao zerar, guarda `result`.
3. **Write-back (CDB único)**

   * **Prioridade**: `LD` → `ADD/SUB` → `MUL/DIV` → `ST`.
   * **LD/ADD/MUL**: faz *broadcast* do `result` no CDB; atualiza registradores com a **tag** correspondente e acorda dependentes (`wake_deps_with_result`).
   * **ST**: grava diretamente em `memv[address % MAX_MEM]`.
4. **Impressões**

   * Mostra tabelas das RS, registradores e *debug* do ciclo.

**Término**: quando todas as instruções foram emitidas (`pc == num_instructions`) **e** não há RS ocupadas.

---

## 7) Estados iniciais (boot da simulação)

* `R0..R31` começam com **valor `i*10`** e `Qi = 0`.
* Memória `memv[0..1023] = 1000 + índice`.
* RS nomeadas (`A1..A5`, `M1..M5`, `L1..L5`, `S1..S5`) e resetadas.

---

## 8) Exemplo mínimo de `instructions.txt`

```txt
# Soma dependente + load e store
ADD R1, R2, R3      # R1 = R2 + R3
MUL R4, R1, R5      # depende de R1 (tag de ADD)
LD  R6, 12(R2)      # carrega de MEM[R2+12]
ST  R6, 0(R1)       # armazena em MEM[R1+0] o valor de R6 (depende de R1 e LD)
NOP
```

---

## 9) Saída: como ler

Em cada ciclo o programa imprime:

* `Issue:` quando uma instrução é emitida.
* `WB:` quando ocorre *write-back* no CDB (ou *commit* de `ST`).
* Tabelas:

  * **ADD/SUB Stations**, **MUL/DIV Stations**, **LOAD Buffers**, **STORE Buffers**

    * Campos principais: `Busy`, `Op`, `Vj/Vk`, `Qj/Qk`, `Dest`, `Rem` (= ciclos restantes), `Addr`, `Res`.
  * **REGISTRADORES**: mostra `R#`, valor e `Qi` (tag pendente).

Dica: observe `Qj/Qk` indo a `0` quando um resultado com a **mesma tag** é publicado.

---

## 10) Limitações (intencionais p/ didática)

* **Um único CDB** (no máx. 1 *write-back* por ciclo).
* Endereço de `LD/ST` **não sofre dependência** do registrador base (é resolvido no *issue* para simplificar).
* Sem reordenação de *commit* em registradores (não há ROB); último *write-back* com a mesma tag vence.
* `DIV` não trata exceções; divisão por zero retorna `0`.
* Sem predição/saltos/controle.
