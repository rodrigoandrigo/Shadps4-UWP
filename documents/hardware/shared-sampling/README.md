# OpenOrbis: amostragem com storage compartilhado

Use `main.cpp` num template OpenOrbis, vinculando GnmDriver e libkernel.
O programa desenha uma superfície 16×16 e compara o hash de RGBA entre a
amostragem simples e a mesma amostragem com um buffer gravável sobre o backing
da imagem. A escrita fica no padding, preservando os texels usados na leitura.

Os casos cobrem LOD explícito/fracionário, gradientes 2D, LOD implícito no PS, bias e clamps, min/mag, mip point/linear, reduction min/max e anisotropia 2×/4×/8×/16×, clamps de LOD fornecidos pelo shader e argumentos `_CD` uniformes no quad. Inclui também gather por componente, consulta de LOD, seis faces de cube, oito modos BC7 e modos BC6 unsigned/signed, incluindo os reservados. Os tipos 9/10 acrescentam os oito modos de wrap/clamp/mirror, point/linear, coordenadas negativas e bordas fixa/customizada com componentes fora de [0,1]. A saída RGBA8 registra a conversão final, sem servir como medida de precisão float32. A textura usa Thin1DThin, pitch 16, quatro mips e cores por nível. `SAMPLING` registra RGBA e hash para comparação com o renderer. Este homebrew não substitui a validação de precisão, fronteiras, arrays/3D ou todos os controles específicos de sampler GCN.

Referências de contratos: [ISA GCN da AMD](https://gpuopen.com/wp-content/uploads/2016/08/AMD_GCN3_Instruction_Set_Architecture_rev1.1.pdf)
e [SampleGrad com clamp mínimo de LOD](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/t3d-samplegrad-s-float-float-float-int-float-).

As variantes 21–22 amostram imediatamente antes da fronteira entre dois texels,
com `TRUNC_COORD` desligado e ligado. As variantes 23–24 usam uma fração
bilinear de 0,75 com `MC_COORD_TRUNC` desligado e ligado. As variantes 25–26
usam `IMAGE_SAMPLE_C_CL` e `IMAGE_SAMPLE_C_D_CL`. A referência de 0,75
separa os resultados dos mips 1 e 2, e o clamp pede no mínimo o mip 2.
Compare as variantes em cada caminho e os hashes entre as duas formas de
binding para verificar o efeito dos controles.

A saída `SAMPLER_PRECISION` usa um alvo RGBA32F e imprime os bits dos quatro
canais, o hash da área desenhada e as duas palavras do descritor de sampler.
Cada caso roda com e sem o alias gravável.

| Grupo | Casos | Variação |
| --- | --- | --- |
| `kind=0` | 0–31 | Quatro valores S1.4 de `LOD_BIAS_SEC` (0, +0,5, +1 e −0,5) para cada código de `PERF_MODULATION`, com LOD explícito igual a 1. |
| `kind=0` | 32–47 | Oito frações entre 1/512 e 8/512 de texel, com `DISABLE_LSB_CEIL` desligado e ligado. |
| `kind=1` | 0–95 | Seis razões de gradientes, cada uma com oito valores de `ANISO_THRESHOLD` e oito de `ANISO_BIAS`, variando um campo por vez. |
| `kind=2` | 0–31 | A mesma matriz de bias secundário e modulação, com LOD calculado no pixel shader. |

O guia Sea Islands identifica os campos, mas não fornece as tabelas de
decisão necessárias para reproduzir todos os resultados. A matriz registra os
valores brutos para distinguir mudança de mip, arredondamento de peso e
mudanças no número ou na posição dos taps.
[Guia de registradores Sea Islands](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/programmer-references/CIK_3D_registers_v2.pdf)
