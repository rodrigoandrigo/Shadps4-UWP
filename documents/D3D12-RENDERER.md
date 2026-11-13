# Renderer D3D12 nativo

Atualizado em 13/11/2026.

## Estado atual

O renderer nativo está habilitado, mas ainda não oferece cobertura completa de jogos.
Vulkan/Dozen e D3D12 nativo são backends distintos; recursos ou correções de um não
implicam suporte automático no outro.

No Xbox Series S, estão corrigidas as cores e a falha de rastros/fundo do PAC-MAN 256.
A correção do fundo usa transformação de viewport diretamente no geometry shader
auxiliar dos retângulos compatíveis. Essa pendência está encerrada; as demais
limitações abaixo continuam independentes dessa correção.

## Arquitetura

O fluxo é GNM/PM4 → Liverpool → `GpuCommandSink` → `D3D12::Rasterizer` →
`CommandRuntime` → D3D12. O boot usa `InitializeNativeGpu` quando o backend nativo
está selecionado, sem traduzir chamadas Vulkan.

O recompilador GCN compartilha IR/SPIR-V com Vulkan. VS/PS/CS usam
SPIRV-Cross → HLSL → DXC; GS/HS/DS usam `CompileNativeSpirv` e `spirv_to_dxil`.
A DLL desse compilador continua necessária para esses estágios.

Runtime e apresentador têm contextos D3D12 próprios. O VideoOut recebe um snapshot
imutável do recurso residente compartilhado, com fallback CPU para representações
incompatíveis. A swapchain do jogo ignora alpha; o overlay usa uma superfície própria.

| Módulo | Responsabilidade |
| --- | --- |
| [d3d12_rasterizer.cpp](../src/video_core/renderer_d3d12/d3d12_rasterizer.cpp) | Tradução de estado guest, seleção de caminhos e integração com memória/Liverpool. |
| [command_runtime.cpp](../src/video_core/renderer_d3d12/command_runtime.cpp), [draw_runtime.cpp](../src/video_core/renderer_d3d12/draw_runtime.cpp) | Gravação, draws, dispatches, clears, feedback e sincronização de recursos. |
| [command_scheduler.cpp](../src/video_core/renderer_d3d12/command_scheduler.cpp) | Contextos em voo, lotes, hazards, fences, leases e coordenação da retenção. |
| [shader_cache.cpp](../src/video_core/renderer_d3d12/shader_cache.cpp), [persistent_cache.cpp](../src/video_core/renderer_d3d12/persistent_cache.cpp) | Variantes, tradução, helpers e caches DXIL/PSO. |
| [resource_cache.cpp](../src/video_core/renderer_d3d12/resource_cache.cpp), [image_cache.cpp](../src/video_core/renderer_d3d12/image_cache.cpp) | Buffers, inputs, imagens, ownership, invalidação e transferências. |
| [image_buffer_alias.cpp](../src/video_core/renderer_d3d12/image_buffer_alias.cpp) | Lowering de storage compartilhado, formatos, amostragem e atômicos. |
| [d3d12_presenter.cpp](../src/uwp/d3d12_presenter.cpp) | VideoOut, composição UWP, redimensionamento, gamma, HDR e captura por LT + RT. |

## Recursos implementados

| Área | Suporte atual |
| --- | --- |
| Comandos | Draw direto/indexado/instanciado, base vertex/instance, ExecuteIndirect com preparação GPU e fallback CPU, dispatch direto/indireto, fill/copy e GDS. |
| Estágios | VS/PS/CS, GS on-chip/off-chip pelo contrato lógico de rings, LS/HS/ES/DS e copy shader. Reflexão DXIL define interfaces e root signatures. |
| Buffers e inputs | Residência persistente, dirty tracking incremental, uploads parciais, fetch inline, índices programáveis e aliases físicos/virtuais, incluindo segmentos repetidos/não lineares nos layouts representáveis. |
| Índices (F03) | Capacidade guest e prefixo mapeado delimitam as leituras sem reduzir a contagem do draw. Índices fora da capacidade/prefixo retornam zero; padding de byte swap não amplia o intervalo válido. |
| Imagens | Views 1D por lowering, 2D/3D, arrays/cube, mips/layers, formatos normalizados/inteiros/float, packed e BC, com tiling CPU/GPU. |
| Aliases e atômicos | Storage compartilhado entre imagens/buffers e estágios; componentes de 8/16 bits preservam vizinhos por operações de 32 bits. Atômicos inteiros/float de 32 bits nos casos representáveis. |
| Amostragem (F06–F08) | Load/store, gather/query, cube, mips, filtros min/mag/mip, reduction e anisotropia manual. Oito modos de endereçamento e custom border tipada; precisão e controles residuais permanecem em F06. |
| Formatos comprimidos | Decodificação BC1–BC7 no lowering raw. Escritas válidas por aliases raw/buffer preservam os bytes; não há recompressão automática de texels em BC. |
| Metadata | CMASK misto por tile 8×8: clear 0 e expanded F, ou D/E/F para MSAA 2×/4×/8× não-DCC. Clear tipado, swizzle/quantização e preservação de tiles/padding. |
| Cor e rasterização | MRT, blend/dual-source, logic ops em alvos inteiros/não inteiros, depth/stencil, depth bounds, culling, até 16 viewports/scissors, provoking vertex e expansões de linhas/pontos, com restrições. |
| Retângulos e quads | GS auxiliar em RectList e quads/quad strips fill compatíveis. Retângulos de viewport único elegíveis transformam o viewport no próprio GS, sem stream-out/replay. Casos complexos mantêm os caminhos específicos. |
| Tessellation | Domínios triangle/quad/isoline e combinações nativas válidas. Pow2 segue o caminho Integer; HS/DS normalizam o tamanho dos patches e fazem lowering de `PatchVertices`. Pares sem conectividade definida continuam rejeitados. |
| Stream-out | Captura de stream 0 com GS, contagem/filled-size obtidos da GPU e eventos sincronizados. Seleção/layout dos descritores e emissão geral ainda são parciais. |
| Apresentação | Snapshot residente, fallback CPU, views UNORM/sRGB compatíveis, gamma/swizzle, blank/last frame, resize/DPI e preenchimento do painel como no Dozen UWP. RGBA4444 preserva canais/alpha também no caminho raw. |
| HDR e FSR | FP16 linear e BT.2020/PQ para scRGB FP16; FSR1 EASU/RCAS opcional em SDR. Não há upscaler temporal implementado. |
| Caches | Variantes antes da tradução GCN com dependências verificadas, DXIL e PSOs em RAM e persistência quando permitida. A identidade inclui os binários do núcleo/compiladores; alterações invalidam entradas incompatíveis. |

A máscara `CB_SHADER_MASK` não é usada para substituir os fatores de alpha por
ONE/ZERO: o blending preserva o alpha exportado pelo shader. As interfaces HLSL
normalizam a posição de `SV_Position` para manter a ligação entre estágios.

## Sincronização, residência e memória

Quatro contextos de trabalho permitem listas, allocators e descritores em voo.
A reutilização depende da fence; leases preservam recursos, pipelines e mapeamentos.
Leitores GPU podem compartilhar dados limpos; escritores exigem resolução e
invalidação dos aliases afetados. Leituras CPU do backing canônico evitam hazards
read/read, mas acessos guest continuam protegidos pelas regras de ownership.

Buffers e imagens dirty permanecem na GPU até um consumidor CPU, alias incompatível,
remapeamento, eviction ou sincronização que exija writeback. Inputs têm cache próprio;
intervalos físicos contíguos reutilizam alocações nativas de 64 KiB sem ler padding guest.
O fetch shader sincroniza sua extensão decodificada, e mudanças de mapeamento
invalidam as identidades de shaders preparados.

`Flush` drena o trabalho sem exigir readback global; `Finish`, sincronizações
explícitas e checkpoints preservam a coerência. O processador compartilhado não
acrescenta sincronização GPU em `ACQUIRE_MEM`/`PFP_SYNC_ME`; o caminho gráfico mantém
`StreamCopyLane::DrainProducer`. Dependências entre contextos, captura residente,
GDS, clears materializados e certos replays ainda podem esperar na CPU.

O orçamento agregado considera RAM, uso/limite UWP e DXGI. A meta de retenção é até
2 GiB em primeiro plano e 64 MiB em segundo plano, reduzida sob pressão. É uma meta
de retenção de caches, não um limite rígido de RAM do processo. Recursos referenciados
ou dirty não descartáveis continuam vivos. Upload/readback e temporários só são
reciclados após suas fences. Snapshots de apresentação são limitados a quatro
alocações em cache por contexto, protegidos também pelas referências do consumidor.

### Limites relevantes

| Recurso | Limite atual |
| --- | --- |
| Buffers | 128 entradas/256 MiB por cache/contexto; inputs têm cache separado. View/grupo compartilhado até 16 MiB. |
| Imagens | Até 521 entradas por contexto (`512 + 8 + 1`); GPU e snapshots CPU sujeitos aos respectivos orçamentos de 256 MiB. Superfície guest até 64 MiB; até 16 mips, 16384×16384 e 2048 layers/depth, conforme formato/dispositivo. |
| Descritores | Por contexto: 512 recursos, 128 samplers, 8 RTVs e 1 DSV; o binding tier pode impor limites menores. |
| Shaders/PSOs | Variantes e DXIL: 512 entradas/64 MiB por cache. PSOs: 128 entradas/32 MiB de chaves por contexto. |
| Persistência | 256 MiB por categoria em disco. Blobs de PSO do driver permanecem desativados em Windows.Xbox; DXIL persistente e caches em RAM são distintos dessa restrição. |
| Transferências | Pool de 256 MiB por contexto; scratch do tiler de 64 MiB. Captura de geometria limitada a 32 MiB por contexto. |
| Indiretos | Até 4096 draws por lote; janela de argumentos até 16 MiB; expansão de até 4 milhões de índices. |
| Apresentação | Contabilidade de frames de 256 MiB, allocator de 512 MiB e FSR separado de 128 MiB; retenção participa do orçamento agregado. |
| LDS/GDS | Spill LDS até 16 MiB por dispatch; GDS de 64 KiB. |

Os limites não são cumulativamente garantidos: orçamento, formatos e capacidades
podem restringir uma operação antes deles. RGB96 tiled permanece rejeitado.

SM6.4 é o mínimo; SM6.6 é usado quando disponível. Em compute, forçar wave64 numa
onda variável exige SM6.6. Em gráficos, operações quad são aceitas; outras operações
cross-lane exigem wave64 fixo. FP64 depende de suporte nativo. Typed UAV load/store,
MSAA, posições programáveis e depth bounds também dependem do dispositivo.

## Trabalho pendente para jogos

Os IDs acompanham lacunas residuais, não uma ordem de implementação. A ausência de
um ID indica que ele não permanece como tarefa funcional inteira; não significa
compatibilidade universal das combinações relacionadas.

### Correção funcional e fidelidade

| ID | Pendência atual |
| --- | --- |
| F01 | Feedback com escrita simultânea em attachments e shader, preservando ordem/efeitos laterais. Leituras de attachments imutáveis são permitidas; sobreposição com escrita continua restrita. |
| F02 | Fetch coerente com conversão de índices/restart e descritores de fetch modificados durante o próprio draw. Dados de vértices/índices já têm caminhos GPU; código e descritores ainda são resolvidos na tradução CPU. |
| F04 | Metadata comprimida/TC-compatible, DCC, HTILE e remapeamento/compressão FMASK guest. Os códigos CMASK não representados continuam rejeitados; não substituir a semântica por valores constantes. |
| F05 | EQAA com samples/fragments diferentes e frequência parcial de shading por sample, preservando interpolação, depth/stencil, blend e efeitos laterais. |
| F06 | Precisão GCN dos filtros e gradientes, controles secundários de anisotropia/LOD, trunc/ceil, seleção coarse por quad e equivalência da anisotropia 3D. |
| F10 | Stream-out sem GS, múltiplos streams/rast stream, descritores/layout exatos e compactação ordenada da emissão entre invocações. |
| F11 | Definir a conectividade guest de pares de domínio/topologia fora do contrato nativo. Continuam rejeitados; o modo de depuração do tessellator não define essa semântica. |
| F12 | Bias por face em point fill, linhas largas/stipple e contornos expandidos, calculado sobre a primitiva original. Fill/wire nativo por face já usa captura única e replay ordenado. |
| F13 | Gate por depth com discard/exportações de depth/coverage/stencil, MSAA/alpha-to-coverage, bounds/storage simultâneos, feedback de depth, expansões, adjacency e certas máscaras de logic ops MRT. |
| F14 | Atualização auxiliar de stencil com MSAA, alpha-to-coverage, efeitos/exportações do PS, expansões, adjacency e múltiplas camadas. Operações lógicas e refs/máscaras por face já têm fallback de sample único/uma camada. |
| F15 | Provoking-last em tessellation, limites de interface e cobertura/interpolação completa de linhas/pontos: bordas, caps, recorte de polígonos, W não unitário e degenerados. O ordinal do GS usa TEXCOORD31, restringindo interfaces que ocupam os 32 atributos. |
| F16 | Estado/reset/acumulação de line stipple entre segmentos e packets; métricas major/full-length, fractional/diamond e W degenerado. Repetição cíclica e fase do recorte com W inicial positivo já são tratadas. |
| F17 | Ampliar a combinação de constantes RGB/alpha sem `AlphaBlendFactorSupported` para MRT e combinações incompatíveis com dual-source guest. O fallback remapeia um alvo elegível ao slot nativo zero. |
| F18 | FP64 por software, modos numéricos restantes, arredondamentos direcionados, denorm FP16/FP64/híbrido, NaN e cross-lane sem wave64 garantido. Não reduzir FP64 silenciosamente a FP32. |
| F19 | Opcodes GCN/IR restantes, DPP/SDWA não cobertos, caminhos de 16/64 bits, semântica geral de append/consume por wave e passagem completa pelos estágios/DXIL SM6.4. Inclui FORK/JOIN, S_BREV_B64 e operações GWS/DS ainda sem suporte. |
| F20 | PM4 válido restante: GET_LOD_STATS, seletores atômicos de COPY_DATA, geração de estatísticas PRIMCOUNT/pipeline, distribuição por RB e casos de SGPR draw index. Oclusão/predicação e COPY_DATA gráfico básico já existem. |
| F21 | Views/formato/layout/alias sem representação no lowering raw, limites das tabelas e combinações dependentes de F01/F04/F05/F06. Storage MSAA escrito/atômico e aliases elegíveis já usam storage compartilhado. |
| F22 | Continuidade após perda de trabalho submetido/estado exclusivamente GPU e recuperação conjunta de renderer/presenter/swapchain. A recuperação conservadora existente não cobre esses casos. |

### Desempenho e escalabilidade

| ID | Estado e trabalho restante |
| --- | --- |
| P01 | Batching e contextos em voo implementados. Reduzir dependências que ainda materializam dados ou esperam na CPU sem enfraquecer hazards/fences. |
| P02 | Residência e dirty tracking incremental implementados. Reduzir readback/retile dos layouts incompatíveis e cópias de inputs nos fallbacks. |
| P03 | Preparação indireta, expansão de índices e byte swap na GPU nos casos elegíveis. Ampliar elegibilidade preservando contagem, SGPRs, restart e ordem de escritas. |
| P04 | Coordenação agregada implementada. Ajustar working sets e retenção sob pressão real do processo/dispositivo, incluindo suspensão/retomada no Xbox. |
| P05 | Tiling GPU para subrecursos, arrays, volumes, BC, planos MSAA e conversões packed elegíveis. Reduzir temporários restantes de depth/stencil e logic ops; manter fallback CPU quando necessário. |
| P06 | Reduzir stalls de tradução/PSO, replays, allocators/descritores e apresentação com profiling de gameplay. A meta de 55 FPS sustentados no PAC-MAN 256 permanece aberta; paridade com Vulkan não está estabelecida. Novas filas/pré-compilação precisam ser justificadas pela carga real. |

## Diagnóstico e recuperação

- **LT + RT**, durante o jogo: captura um intervalo completo de VideoOut no backend
  nativo e salva `videoout.png/bin`, `backbuffer.png/bin` e `manifest.json` em
  `LocalState/shadPS4/screenshots/presentation/native-*`. Inclui estados de
  draws/clears, bindings e HLSL; os campos `direct_rectangle_viewport` e
  `geometry_capture` identificam o caminho dos retângulos. A captura altera o timing
  e não deve ser usada como medição normal de desempenho.
- A captura limita o payload a 128 MiB; o trace é limitado a 4096 eventos/4 MiB,
  com HLSL de até 1 MiB por programa. O manifest informa truncamento.
- `GAME_PROFILE=1`: contadores/tempos CPU e timestamps GPU. Os tempos são inclusivos
  e cumulativos; não somar etapas aninhadas ou relatórios sucessivos.
- `GAME_D3D_DEBUG=1`: habilita a camada de diagnóstico antes de criar o dispositivo.
  Falhas de pipeline incluem mensagens da info queue; DRED é consultado em device loss.
- `SHADPS4_D3D12_RECOVERY=1`: recuperação conservadora, desligada por padrão. Faz
  checkpoints sincronizados, desabilita batching assíncrono e permite uma tentativa
  por sessão na thread PM4. Recusa replay após submissão, escrita CPU, operação já
  concluída ou checkpoint inválido. Pode introduzir stalls e não garante continuidade.

## Aproveitamento do renderer D3D12 de pt-pc-main

Permanecem os heaps de descritores reutilizados por contexto após fences, uploads
incrementais de buffers, timestamps dos batches, verificação de remoção do dispositivo
durante esperas, drenagem na destruição e relatórios de orçamento/DRED. Essas partes
integram os módulos atuais; filas ou políticas de outros emuladores não definem os
limites deste renderer.

## Critério de conclusão

É necessário resolver ou delimitar por contrato as lacunas funcionais, manter
coerência/efeitos laterais e conferir o comportamento em jogos e cenários variados
no PC e Xbox. A cobertura deve incluir transições, sessões prolongadas, resize/DPI,
HDR físico, carga/descarga, stop, suspensão, pressão de memória e falhas de GPU.

Desempenho deve considerar frame time, stalls e memória em gameplay reproduzível.
Chegar ao menu ou corrigir uma cena não encerra a cobertura geral. FSR1, reconstrução
temporal, frame generation e uma copy queue separada não são requisitos isolados de
correção gráfica.
