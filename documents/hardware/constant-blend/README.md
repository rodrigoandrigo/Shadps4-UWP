# OpenOrbis: mistura de constantes RGB e alpha

Fonte GnmDriver/libkernel: 20 combinacoes de constantes RGB/alpha e inversos, com ADD/SUBTRACT/MIN/MAX/REVERSE_SUBTRACT. Compara o pixel central RGBA8 com a equacao CPU, tolerancia de um nivel UNORM. Alpha usa equacao separada. Validacao local somente sintatica; nao cobre MRT, MSAA, formatos float ou efeitos de storage.

## Source alpha with a masked alpha channel

Two additional cases export alpha 0.25, write RGB only, and repeat the draw 16 times over initialized pixels. They use shader masks 15 and 7 with SRC_ALPHA / INV_SRC_ALPHA blending. Both record and check the bounded recurrence against the exported alpha; the shader mask must not be used as proof that the shader omitted its alpha export. These added OpenOrbis cases have not been built or executed on hardware.
