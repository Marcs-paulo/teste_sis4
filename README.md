# SIS — Teste 3/4 (sensor raso + coentro)

Firmware da ESP32-C6 que lê dois sensores de umidade rasos (~5 cm), controla a
eletroválvula e manda os dados para uma planilha do Google. Pode ser
configurado e **atualizado pela internet**, de qualquer lugar.

| Pasta / arquivo | O que é |
|---|---|
| `src/main.cpp` | firmware da placa |
| `include/segredos.exemplo.h` | modelo do arquivo com WiFi e URLs (o real **não** fica no GitHub) |
| `platformio.ini` | projeto PlatformIO (env de cabo + env `release` para a internet) |
| `apps_script_esp.gs` | Apps Script que recebe os dados e guarda a configuração remota |
| `painel/` | painel de gráficos (somente leitura) e gerador da página compartilhável |

---

## Para quem vai até a placa: gravar pelo cabo (uma vez)

Esta gravação instala a versão 2, a primeira que aceita atualização pela
internet. Depois dela, ninguém precisa voltar ao local para atualizar.

1. Instale o **VS Code** com a extensão **PlatformIO** e o **Git**.
2. Baixe este repositório: botão verde **Code → Download ZIP** (e descompacte),
   ou `git clone https://github.com/Marcs-paulo/teste_sis4.git`.
3. Coloque na pasta `include/` o arquivo **`segredos.h`** que o dono te mandou
   por mensagem. Sem ele a compilação para com o aviso
   "Falta include/segredos.h".
4. No VS Code: **File → Open Folder** e escolha a pasta do projeto.
   Espere o PlatformIO terminar de preparar (barra de baixo).
5. Ligue a ESP no cabo USB e clique em **→ (Upload)** na barra de baixo.
   Espere aparecer `SUCCESS`.
6. ⚠️ **Não segure o botão da placa enquanto ela liga.** Segurar o botão no boot
   apaga o teste e começa um "Dia 1" novo. Sem segurar, ela continua o cultivo
   de onde parou.
7. Confira: o LED fica **ciano** (observando entre regas) ou **verde** (regando).
   Vermelho = sem WiFi.

---

## Para o dono: controlar a distância

Todos os comandos são abertos no navegador (celular serve). Base:

```
https://script.google.com/macros/s/<ID-DO-SCRIPT>/exec
```

| O que fazer | Acrescente depois de `/exec` |
|---|---|
| Ver a configuração e a versão em uso | nada |
| Trocar teste/modo | `?cmd=set&token=<TOKEN>&teste=4&modo=cultivo` |
| Ajustar a rega | `?cmd=set&token=<TOKEN>&limiar=1150&maxH=48&minH=3` |
| Atualizar o firmware | `?cmd=set&token=<TOKEN>&fw=3` |

A placa consulta a configuração a cada 5 minutos. A resposta mostra
`fwAtual` (versão rodando) e `visto` (última vez que a placa falou).

Faixas aceitas: `limiar` 900–2400, `maxH` 6–96, `minH` 1–48 (e `minH` menor
que `maxH`). Valores fora disso são ignorados e aparecem em `avisos`.

### Apps Script (uma vez)

1. Cole `apps_script_esp.gs` no projeto do Apps Script que a placa já usa e salve.
2. **Configurações do projeto (engrenagem) → Propriedades do script →**
   adicionar `TOKEN` com o valor do token. Sem isso, todo comando responde
   "token invalido".
3. **Implantar → Gerenciar implantações → lápis → Nova versão → Implantar.**
   A URL continua a mesma.

---

## Publicar uma versão nova pela internet

1. Faça as mudanças em `src/main.cpp` e **aumente `FW_VERSAO`** (ex.: 2 → 3).
2. Compile o firmware público (sem senhas):
   ```
   pio run -e release
   ```
   O arquivo sai em `.pio/build/release/firmware.bin`.
3. No GitHub: **Releases → Draft a new release**. Tag **`fw-3`** (o mesmo
   número de `FW_VERSAO`). Arraste o `firmware.bin` (o nome tem que ser
   exatamente esse) e clique em **Publish release**.
4. Mande `?cmd=set&token=<TOKEN>&fw=3`.
5. Em até 5 minutos a placa baixa e reinicia. Em ~15 minutos a resposta da
   configuração deve mostrar `"fwAtual":3`.

### Proteções

- A atualização **nunca começa durante uma rega**; a válvula fica fechada
  durante o download, e os dados guardados são enviados antes.
- A versão nova fica **em prova**. Se ela não conseguir falar com o Apps
  Script em 20 minutos, ou se reiniciar 3 vezes seguidas, a placa **volta
  sozinha para a versão anterior** e não tenta mais aquela versão.
  Para tentar de novo, publique com outro número (`fw-4`).
- O download falho é tentado até 3 vezes (uma a cada 5 minutos).
- Toda versão nova precisa manter esse código de prova/volta. Uma versão que
  não tiver isso e não ligar só se resolve com cabo.
- O firmware `release` não leva a senha do WiFi nem a URL do script: ele usa o
  que a gravação por cabo deixou salvo na placa. Por isso ele **só serve para
  atualizar uma placa que já recebeu a versão de cabo**.

### OTA local (mesma rede WiFi da placa)

Quem estiver no local também pode gravar sem cabo:

```
pio run -t upload --upload-port sis-teste3.local
```

(senha do OTA local: a de `segredos.h`).
