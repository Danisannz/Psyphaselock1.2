# DaniSan PsyPhase Lock v1.2 — alinhador de fase kick/bass (VST3)

Plugin para o **grupo** onde kick e bass tocam juntos (darkpsy / forest, 150–200 BPM).
Mede a fase do kick e do bass na frequência do bass e gira a fase do bass (all-pass + polaridade)
para ficarem em fase no início de cada nota. Latência fixa: **65 ms** (o host compensa via PDC).

## Roteamento (importante)
Fase relativa não pode ser medida só da soma kick+bass, então o plugin usa o **sidechain** como referência do kick.

* **Modo GROUP** (padrão): plugin no grupo (kick+bass). Mande uma cópia do kick (send *pré-fader*,
  mesmo nível) para o **sidechain** do plugin. O plugin faz `bass = grupo − kick`, gira só o bass e
  deixa todo o resto como está. Se o send não for idêntico ao kick do grupo, acerte com **KICK TRIM**.
* **Modo TRACK** (GROUP desligado): plugin na faixa do bass, kick no sidechain.

## Parâmetros
| Controle | Função |
|---|---|
| FOCUS | Frequência de análise (40–100 Hz). Ajuste para a nota raiz do bass / fundamental do kick |
| TARGET | Fase desejada do bass em relação ao kick (0° = em fase) |
| AUTO | Liga o alinhamento automático por nota |
| MANUAL | Rotação manual 0–360° (só com AUTO desligado) |
| THRESHOLD | Nível a partir do qual kick/bass contam como "tocando" |
| SETTLE | Espera extra antes de medir (use 30–40 ms se o bass cai junto com o início do kick, onde o pitch ainda desce) |
| MIX | Dry/wet da correção |
| KICK TRIM | Ajuste de nível do kick do sidechain (modo GROUP) |

**Osciloscópio sincronizado no BPM do host:** mostra 1 batida (1 e & a = as 4 semicolcheias) e sobrepõe as últimas 8 batidas.
Magenta = kick (sidechain), ciano = bass depois do plugin, laranja = bass antes (botão BEFORE). Se todas as notas travam,
os traços ficam empilhados. FREEZE congela a tela. GHOST: notas de bass sem kick por baixo seguem a continuação da fase do último kick.

## Compilar
Requisitos: CMake ≥ 3.22, git, Visual Studio 2022 (Desktop C++) ou Xcode, e internet na 1ª vez (baixa o JUCE 8.0.4).
```
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```
O VST3 fica em `build/PsyPhaseLock_artefacts/Release/VST3/PsyPhase Lock.vst3`
(copie para `C:\Program Files\Common Files\VST3`). No Mac: `cmake -B build -G Xcode`.

## Novidades da v1.2
* **Detector de ritmo** (kick + bass): tempo detectado pelo próprio kick, cada nota do bass é encaixada na grade de 16avos.
  Mostra **STRAIGHT** (reto), **TENSION** (nota a mais / a menos / empurrada) ou **FILL** (virada), o padrão das 4 semicolcheias
  e a % de batidas em que o bass corre em 16avos. Assume kick em toda batida (4 no chão).
* **Medição de fase mais precisa**: bass medido com janela Hann de 2 períodos; referência do kick tirada depois do sweep de pitch
  (e prevista a partir do kick anterior para notas que entram junto com o kick).
* **AUTO TRIM** (modo GROUP): mede sozinho o nível do kick do sidechain dentro do grupo. Só vale se as batidas concordarem;
  senão usa o KICK TRIM manual.
* **STABLE**: protege contra nota isolada corrompida usando o histórico daquele slot. Não muda nada em material limpo.
* Correção aplicada no vão antes de cada nota (nota que entra já com a fase certa).

## Limites
* No início de cada nota (primeiros ~10 ms) o próprio filtro all-pass ainda está assentando, então a fase ali pode diferir
  até ~20° do valor final; depois disso fica em ~1-2°.
* Kick e bass precisam estar na mesma nota/pitch (use FOCUS). Notas de bass com pitch diferente do FOCUS (viradas) não têm fase útil.
* Medido só com sinais sintéticos, não com o seu projeto.

## Testes do motor de DSP (sem JUCE)
```
cd Tests
./run_tests.sh      # precisa de g++ e python3 com numpy
```

## Repositório novo (GitHub) - passo a passo
1. github.com -> **New repository** (nome qualquer, ex.: `danisan-psyphase`, pode ser privado).
2. Na página do repositório vazio: **uploading an existing file**.
3. Extraia este zip e arraste **todo o conteúdo** para a página (CMakeLists.txt, README.md, pasta `Source`, pasta `Tests`
   e a pasta oculta `.github`). Confira se aparecem `.github/workflows/build.yml` e `Source/LogoData.h` na lista antes de commitar.
4. **Commit changes**. Aba **Actions** -> o build roda sozinho (~5 min).
5. Clique na execução verde -> **Artifacts** -> baixe `DaniSan-PsyPhaseLock-VST3-build-N`.
6. Copie a pasta `DaniSan PsyPhase Lock.vst3` para `C:\Program Files\Common Files\VST3`, **apague o `PsyPhase Lock.vst3` antigo** e reescaneie os plugins.

O plugin novo tem nome e ID diferentes do anterior (aparece como "DaniSan PsyPhase Lock"), então a DAW nunca mistura com a versão velha.
Se o canto superior direito mostrar `v1.1 | Dani San`, é o build novo.
