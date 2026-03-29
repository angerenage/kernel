.intel_syntax noprefix

.section .text
.extern x86_64_handle_interrupt

.macro INTERRUPT_STUB vector has_error_code
.global x86_64_interrupt_stub_\vector
x86_64_interrupt_stub_\vector:
	.if \has_error_code == 0
		push 0
	.endif
	push \vector
	jmp x86_64_interrupt_common
.endm

.global x86_64_interrupt_common
x86_64_interrupt_common:
	push r15
	push r14
	push r13
	push r12
	push r11
	push r10
	push r9
	push r8
	push rsi
	push rdi
	push rbp
	push rdx
	push rcx
	push rbx
	push rax

	mov rdi, rsp
	call x86_64_handle_interrupt

	pop rax
	pop rbx
	pop rcx
	pop rdx
	pop rbp
	pop rdi
	pop rsi
	pop r8
	pop r9
	pop r10
	pop r11
	pop r12
	pop r13
	pop r14
	pop r15
	add rsp, 16
	iretq

INTERRUPT_STUB 0, 0
INTERRUPT_STUB 1, 0
INTERRUPT_STUB 2, 0
INTERRUPT_STUB 3, 0
INTERRUPT_STUB 4, 0
INTERRUPT_STUB 5, 0
INTERRUPT_STUB 6, 0
INTERRUPT_STUB 7, 0
INTERRUPT_STUB 8, 1
INTERRUPT_STUB 9, 0
INTERRUPT_STUB 10, 1
INTERRUPT_STUB 11, 1
INTERRUPT_STUB 12, 1
INTERRUPT_STUB 13, 1
INTERRUPT_STUB 14, 1
INTERRUPT_STUB 15, 0
INTERRUPT_STUB 16, 0
INTERRUPT_STUB 17, 1
INTERRUPT_STUB 18, 0
INTERRUPT_STUB 19, 0
INTERRUPT_STUB 20, 0
INTERRUPT_STUB 21, 1
INTERRUPT_STUB 22, 0
INTERRUPT_STUB 23, 0
INTERRUPT_STUB 24, 0
INTERRUPT_STUB 25, 0
INTERRUPT_STUB 26, 0
INTERRUPT_STUB 27, 0
INTERRUPT_STUB 28, 0
INTERRUPT_STUB 29, 0
INTERRUPT_STUB 30, 1
INTERRUPT_STUB 31, 0
INTERRUPT_STUB 32, 0
INTERRUPT_STUB 33, 0
INTERRUPT_STUB 34, 0
INTERRUPT_STUB 35, 0
INTERRUPT_STUB 36, 0
INTERRUPT_STUB 37, 0
INTERRUPT_STUB 38, 0
INTERRUPT_STUB 39, 0
INTERRUPT_STUB 40, 0
INTERRUPT_STUB 41, 0
INTERRUPT_STUB 42, 0
INTERRUPT_STUB 43, 0
INTERRUPT_STUB 44, 0
INTERRUPT_STUB 45, 0
INTERRUPT_STUB 46, 0
INTERRUPT_STUB 47, 0
INTERRUPT_STUB 48, 0
INTERRUPT_STUB 49, 0
INTERRUPT_STUB 50, 0
INTERRUPT_STUB 51, 0
INTERRUPT_STUB 52, 0
INTERRUPT_STUB 53, 0
INTERRUPT_STUB 54, 0
INTERRUPT_STUB 55, 0
INTERRUPT_STUB 56, 0
INTERRUPT_STUB 57, 0
INTERRUPT_STUB 58, 0
INTERRUPT_STUB 59, 0
INTERRUPT_STUB 60, 0
INTERRUPT_STUB 61, 0
INTERRUPT_STUB 62, 0
INTERRUPT_STUB 63, 0
INTERRUPT_STUB 64, 0
INTERRUPT_STUB 65, 0
INTERRUPT_STUB 66, 0
INTERRUPT_STUB 67, 0
INTERRUPT_STUB 68, 0
INTERRUPT_STUB 69, 0
INTERRUPT_STUB 70, 0
INTERRUPT_STUB 71, 0
INTERRUPT_STUB 72, 0
INTERRUPT_STUB 73, 0
INTERRUPT_STUB 74, 0
INTERRUPT_STUB 75, 0
INTERRUPT_STUB 76, 0
INTERRUPT_STUB 77, 0
INTERRUPT_STUB 78, 0
INTERRUPT_STUB 79, 0
INTERRUPT_STUB 80, 0
INTERRUPT_STUB 81, 0
INTERRUPT_STUB 82, 0
INTERRUPT_STUB 83, 0
INTERRUPT_STUB 84, 0
INTERRUPT_STUB 85, 0
INTERRUPT_STUB 86, 0
INTERRUPT_STUB 87, 0
INTERRUPT_STUB 88, 0
INTERRUPT_STUB 89, 0
INTERRUPT_STUB 90, 0
INTERRUPT_STUB 91, 0
INTERRUPT_STUB 92, 0
INTERRUPT_STUB 93, 0
INTERRUPT_STUB 94, 0
INTERRUPT_STUB 95, 0
INTERRUPT_STUB 96, 0
INTERRUPT_STUB 97, 0
INTERRUPT_STUB 98, 0
INTERRUPT_STUB 99, 0
INTERRUPT_STUB 100, 0
INTERRUPT_STUB 101, 0
INTERRUPT_STUB 102, 0
INTERRUPT_STUB 103, 0
INTERRUPT_STUB 104, 0
INTERRUPT_STUB 105, 0
INTERRUPT_STUB 106, 0
INTERRUPT_STUB 107, 0
INTERRUPT_STUB 108, 0
INTERRUPT_STUB 109, 0
INTERRUPT_STUB 110, 0
INTERRUPT_STUB 111, 0
INTERRUPT_STUB 112, 0
INTERRUPT_STUB 113, 0
INTERRUPT_STUB 114, 0
INTERRUPT_STUB 115, 0
INTERRUPT_STUB 116, 0
INTERRUPT_STUB 117, 0
INTERRUPT_STUB 118, 0
INTERRUPT_STUB 119, 0
INTERRUPT_STUB 120, 0
INTERRUPT_STUB 121, 0
INTERRUPT_STUB 122, 0
INTERRUPT_STUB 123, 0
INTERRUPT_STUB 124, 0
INTERRUPT_STUB 125, 0
INTERRUPT_STUB 126, 0
INTERRUPT_STUB 127, 0
INTERRUPT_STUB 128, 0
INTERRUPT_STUB 129, 0
INTERRUPT_STUB 130, 0
INTERRUPT_STUB 131, 0
INTERRUPT_STUB 132, 0
INTERRUPT_STUB 133, 0
INTERRUPT_STUB 134, 0
INTERRUPT_STUB 135, 0
INTERRUPT_STUB 136, 0
INTERRUPT_STUB 137, 0
INTERRUPT_STUB 138, 0
INTERRUPT_STUB 139, 0
INTERRUPT_STUB 140, 0
INTERRUPT_STUB 141, 0
INTERRUPT_STUB 142, 0
INTERRUPT_STUB 143, 0
INTERRUPT_STUB 144, 0
INTERRUPT_STUB 145, 0
INTERRUPT_STUB 146, 0
INTERRUPT_STUB 147, 0
INTERRUPT_STUB 148, 0
INTERRUPT_STUB 149, 0
INTERRUPT_STUB 150, 0
INTERRUPT_STUB 151, 0
INTERRUPT_STUB 152, 0
INTERRUPT_STUB 153, 0
INTERRUPT_STUB 154, 0
INTERRUPT_STUB 155, 0
INTERRUPT_STUB 156, 0
INTERRUPT_STUB 157, 0
INTERRUPT_STUB 158, 0
INTERRUPT_STUB 159, 0
INTERRUPT_STUB 160, 0
INTERRUPT_STUB 161, 0
INTERRUPT_STUB 162, 0
INTERRUPT_STUB 163, 0
INTERRUPT_STUB 164, 0
INTERRUPT_STUB 165, 0
INTERRUPT_STUB 166, 0
INTERRUPT_STUB 167, 0
INTERRUPT_STUB 168, 0
INTERRUPT_STUB 169, 0
INTERRUPT_STUB 170, 0
INTERRUPT_STUB 171, 0
INTERRUPT_STUB 172, 0
INTERRUPT_STUB 173, 0
INTERRUPT_STUB 174, 0
INTERRUPT_STUB 175, 0
INTERRUPT_STUB 176, 0
INTERRUPT_STUB 177, 0
INTERRUPT_STUB 178, 0
INTERRUPT_STUB 179, 0
INTERRUPT_STUB 180, 0
INTERRUPT_STUB 181, 0
INTERRUPT_STUB 182, 0
INTERRUPT_STUB 183, 0
INTERRUPT_STUB 184, 0
INTERRUPT_STUB 185, 0
INTERRUPT_STUB 186, 0
INTERRUPT_STUB 187, 0
INTERRUPT_STUB 188, 0
INTERRUPT_STUB 189, 0
INTERRUPT_STUB 190, 0
INTERRUPT_STUB 191, 0
INTERRUPT_STUB 192, 0
INTERRUPT_STUB 193, 0
INTERRUPT_STUB 194, 0
INTERRUPT_STUB 195, 0
INTERRUPT_STUB 196, 0
INTERRUPT_STUB 197, 0
INTERRUPT_STUB 198, 0
INTERRUPT_STUB 199, 0
INTERRUPT_STUB 200, 0
INTERRUPT_STUB 201, 0
INTERRUPT_STUB 202, 0
INTERRUPT_STUB 203, 0
INTERRUPT_STUB 204, 0
INTERRUPT_STUB 205, 0
INTERRUPT_STUB 206, 0
INTERRUPT_STUB 207, 0
INTERRUPT_STUB 208, 0
INTERRUPT_STUB 209, 0
INTERRUPT_STUB 210, 0
INTERRUPT_STUB 211, 0
INTERRUPT_STUB 212, 0
INTERRUPT_STUB 213, 0
INTERRUPT_STUB 214, 0
INTERRUPT_STUB 215, 0
INTERRUPT_STUB 216, 0
INTERRUPT_STUB 217, 0
INTERRUPT_STUB 218, 0
INTERRUPT_STUB 219, 0
INTERRUPT_STUB 220, 0
INTERRUPT_STUB 221, 0
INTERRUPT_STUB 222, 0
INTERRUPT_STUB 223, 0
INTERRUPT_STUB 224, 0
INTERRUPT_STUB 225, 0
INTERRUPT_STUB 226, 0
INTERRUPT_STUB 227, 0
INTERRUPT_STUB 228, 0
INTERRUPT_STUB 229, 0
INTERRUPT_STUB 230, 0
INTERRUPT_STUB 231, 0
INTERRUPT_STUB 232, 0
INTERRUPT_STUB 233, 0
INTERRUPT_STUB 234, 0
INTERRUPT_STUB 235, 0
INTERRUPT_STUB 236, 0
INTERRUPT_STUB 237, 0
INTERRUPT_STUB 238, 0
INTERRUPT_STUB 239, 0
INTERRUPT_STUB 240, 0
INTERRUPT_STUB 241, 0
INTERRUPT_STUB 242, 0
INTERRUPT_STUB 243, 0
INTERRUPT_STUB 244, 0
INTERRUPT_STUB 245, 0
INTERRUPT_STUB 246, 0
INTERRUPT_STUB 247, 0
INTERRUPT_STUB 248, 0
INTERRUPT_STUB 249, 0
INTERRUPT_STUB 250, 0
INTERRUPT_STUB 251, 0
INTERRUPT_STUB 252, 0
INTERRUPT_STUB 253, 0
INTERRUPT_STUB 254, 0
INTERRUPT_STUB 255, 0

.global x86_64_interrupt_stub_table
x86_64_interrupt_stub_table:
	.quad x86_64_interrupt_stub_0
	.quad x86_64_interrupt_stub_1
	.quad x86_64_interrupt_stub_2
	.quad x86_64_interrupt_stub_3
	.quad x86_64_interrupt_stub_4
	.quad x86_64_interrupt_stub_5
	.quad x86_64_interrupt_stub_6
	.quad x86_64_interrupt_stub_7
	.quad x86_64_interrupt_stub_8
	.quad x86_64_interrupt_stub_9
	.quad x86_64_interrupt_stub_10
	.quad x86_64_interrupt_stub_11
	.quad x86_64_interrupt_stub_12
	.quad x86_64_interrupt_stub_13
	.quad x86_64_interrupt_stub_14
	.quad x86_64_interrupt_stub_15
	.quad x86_64_interrupt_stub_16
	.quad x86_64_interrupt_stub_17
	.quad x86_64_interrupt_stub_18
	.quad x86_64_interrupt_stub_19
	.quad x86_64_interrupt_stub_20
	.quad x86_64_interrupt_stub_21
	.quad x86_64_interrupt_stub_22
	.quad x86_64_interrupt_stub_23
	.quad x86_64_interrupt_stub_24
	.quad x86_64_interrupt_stub_25
	.quad x86_64_interrupt_stub_26
	.quad x86_64_interrupt_stub_27
	.quad x86_64_interrupt_stub_28
	.quad x86_64_interrupt_stub_29
	.quad x86_64_interrupt_stub_30
	.quad x86_64_interrupt_stub_31
	.quad x86_64_interrupt_stub_32
	.quad x86_64_interrupt_stub_33
	.quad x86_64_interrupt_stub_34
	.quad x86_64_interrupt_stub_35
	.quad x86_64_interrupt_stub_36
	.quad x86_64_interrupt_stub_37
	.quad x86_64_interrupt_stub_38
	.quad x86_64_interrupt_stub_39
	.quad x86_64_interrupt_stub_40
	.quad x86_64_interrupt_stub_41
	.quad x86_64_interrupt_stub_42
	.quad x86_64_interrupt_stub_43
	.quad x86_64_interrupt_stub_44
	.quad x86_64_interrupt_stub_45
	.quad x86_64_interrupt_stub_46
	.quad x86_64_interrupt_stub_47
	.quad x86_64_interrupt_stub_48
	.quad x86_64_interrupt_stub_49
	.quad x86_64_interrupt_stub_50
	.quad x86_64_interrupt_stub_51
	.quad x86_64_interrupt_stub_52
	.quad x86_64_interrupt_stub_53
	.quad x86_64_interrupt_stub_54
	.quad x86_64_interrupt_stub_55
	.quad x86_64_interrupt_stub_56
	.quad x86_64_interrupt_stub_57
	.quad x86_64_interrupt_stub_58
	.quad x86_64_interrupt_stub_59
	.quad x86_64_interrupt_stub_60
	.quad x86_64_interrupt_stub_61
	.quad x86_64_interrupt_stub_62
	.quad x86_64_interrupt_stub_63
	.quad x86_64_interrupt_stub_64
	.quad x86_64_interrupt_stub_65
	.quad x86_64_interrupt_stub_66
	.quad x86_64_interrupt_stub_67
	.quad x86_64_interrupt_stub_68
	.quad x86_64_interrupt_stub_69
	.quad x86_64_interrupt_stub_70
	.quad x86_64_interrupt_stub_71
	.quad x86_64_interrupt_stub_72
	.quad x86_64_interrupt_stub_73
	.quad x86_64_interrupt_stub_74
	.quad x86_64_interrupt_stub_75
	.quad x86_64_interrupt_stub_76
	.quad x86_64_interrupt_stub_77
	.quad x86_64_interrupt_stub_78
	.quad x86_64_interrupt_stub_79
	.quad x86_64_interrupt_stub_80
	.quad x86_64_interrupt_stub_81
	.quad x86_64_interrupt_stub_82
	.quad x86_64_interrupt_stub_83
	.quad x86_64_interrupt_stub_84
	.quad x86_64_interrupt_stub_85
	.quad x86_64_interrupt_stub_86
	.quad x86_64_interrupt_stub_87
	.quad x86_64_interrupt_stub_88
	.quad x86_64_interrupt_stub_89
	.quad x86_64_interrupt_stub_90
	.quad x86_64_interrupt_stub_91
	.quad x86_64_interrupt_stub_92
	.quad x86_64_interrupt_stub_93
	.quad x86_64_interrupt_stub_94
	.quad x86_64_interrupt_stub_95
	.quad x86_64_interrupt_stub_96
	.quad x86_64_interrupt_stub_97
	.quad x86_64_interrupt_stub_98
	.quad x86_64_interrupt_stub_99
	.quad x86_64_interrupt_stub_100
	.quad x86_64_interrupt_stub_101
	.quad x86_64_interrupt_stub_102
	.quad x86_64_interrupt_stub_103
	.quad x86_64_interrupt_stub_104
	.quad x86_64_interrupt_stub_105
	.quad x86_64_interrupt_stub_106
	.quad x86_64_interrupt_stub_107
	.quad x86_64_interrupt_stub_108
	.quad x86_64_interrupt_stub_109
	.quad x86_64_interrupt_stub_110
	.quad x86_64_interrupt_stub_111
	.quad x86_64_interrupt_stub_112
	.quad x86_64_interrupt_stub_113
	.quad x86_64_interrupt_stub_114
	.quad x86_64_interrupt_stub_115
	.quad x86_64_interrupt_stub_116
	.quad x86_64_interrupt_stub_117
	.quad x86_64_interrupt_stub_118
	.quad x86_64_interrupt_stub_119
	.quad x86_64_interrupt_stub_120
	.quad x86_64_interrupt_stub_121
	.quad x86_64_interrupt_stub_122
	.quad x86_64_interrupt_stub_123
	.quad x86_64_interrupt_stub_124
	.quad x86_64_interrupt_stub_125
	.quad x86_64_interrupt_stub_126
	.quad x86_64_interrupt_stub_127
	.quad x86_64_interrupt_stub_128
	.quad x86_64_interrupt_stub_129
	.quad x86_64_interrupt_stub_130
	.quad x86_64_interrupt_stub_131
	.quad x86_64_interrupt_stub_132
	.quad x86_64_interrupt_stub_133
	.quad x86_64_interrupt_stub_134
	.quad x86_64_interrupt_stub_135
	.quad x86_64_interrupt_stub_136
	.quad x86_64_interrupt_stub_137
	.quad x86_64_interrupt_stub_138
	.quad x86_64_interrupt_stub_139
	.quad x86_64_interrupt_stub_140
	.quad x86_64_interrupt_stub_141
	.quad x86_64_interrupt_stub_142
	.quad x86_64_interrupt_stub_143
	.quad x86_64_interrupt_stub_144
	.quad x86_64_interrupt_stub_145
	.quad x86_64_interrupt_stub_146
	.quad x86_64_interrupt_stub_147
	.quad x86_64_interrupt_stub_148
	.quad x86_64_interrupt_stub_149
	.quad x86_64_interrupt_stub_150
	.quad x86_64_interrupt_stub_151
	.quad x86_64_interrupt_stub_152
	.quad x86_64_interrupt_stub_153
	.quad x86_64_interrupt_stub_154
	.quad x86_64_interrupt_stub_155
	.quad x86_64_interrupt_stub_156
	.quad x86_64_interrupt_stub_157
	.quad x86_64_interrupt_stub_158
	.quad x86_64_interrupt_stub_159
	.quad x86_64_interrupt_stub_160
	.quad x86_64_interrupt_stub_161
	.quad x86_64_interrupt_stub_162
	.quad x86_64_interrupt_stub_163
	.quad x86_64_interrupt_stub_164
	.quad x86_64_interrupt_stub_165
	.quad x86_64_interrupt_stub_166
	.quad x86_64_interrupt_stub_167
	.quad x86_64_interrupt_stub_168
	.quad x86_64_interrupt_stub_169
	.quad x86_64_interrupt_stub_170
	.quad x86_64_interrupt_stub_171
	.quad x86_64_interrupt_stub_172
	.quad x86_64_interrupt_stub_173
	.quad x86_64_interrupt_stub_174
	.quad x86_64_interrupt_stub_175
	.quad x86_64_interrupt_stub_176
	.quad x86_64_interrupt_stub_177
	.quad x86_64_interrupt_stub_178
	.quad x86_64_interrupt_stub_179
	.quad x86_64_interrupt_stub_180
	.quad x86_64_interrupt_stub_181
	.quad x86_64_interrupt_stub_182
	.quad x86_64_interrupt_stub_183
	.quad x86_64_interrupt_stub_184
	.quad x86_64_interrupt_stub_185
	.quad x86_64_interrupt_stub_186
	.quad x86_64_interrupt_stub_187
	.quad x86_64_interrupt_stub_188
	.quad x86_64_interrupt_stub_189
	.quad x86_64_interrupt_stub_190
	.quad x86_64_interrupt_stub_191
	.quad x86_64_interrupt_stub_192
	.quad x86_64_interrupt_stub_193
	.quad x86_64_interrupt_stub_194
	.quad x86_64_interrupt_stub_195
	.quad x86_64_interrupt_stub_196
	.quad x86_64_interrupt_stub_197
	.quad x86_64_interrupt_stub_198
	.quad x86_64_interrupt_stub_199
	.quad x86_64_interrupt_stub_200
	.quad x86_64_interrupt_stub_201
	.quad x86_64_interrupt_stub_202
	.quad x86_64_interrupt_stub_203
	.quad x86_64_interrupt_stub_204
	.quad x86_64_interrupt_stub_205
	.quad x86_64_interrupt_stub_206
	.quad x86_64_interrupt_stub_207
	.quad x86_64_interrupt_stub_208
	.quad x86_64_interrupt_stub_209
	.quad x86_64_interrupt_stub_210
	.quad x86_64_interrupt_stub_211
	.quad x86_64_interrupt_stub_212
	.quad x86_64_interrupt_stub_213
	.quad x86_64_interrupt_stub_214
	.quad x86_64_interrupt_stub_215
	.quad x86_64_interrupt_stub_216
	.quad x86_64_interrupt_stub_217
	.quad x86_64_interrupt_stub_218
	.quad x86_64_interrupt_stub_219
	.quad x86_64_interrupt_stub_220
	.quad x86_64_interrupt_stub_221
	.quad x86_64_interrupt_stub_222
	.quad x86_64_interrupt_stub_223
	.quad x86_64_interrupt_stub_224
	.quad x86_64_interrupt_stub_225
	.quad x86_64_interrupt_stub_226
	.quad x86_64_interrupt_stub_227
	.quad x86_64_interrupt_stub_228
	.quad x86_64_interrupt_stub_229
	.quad x86_64_interrupt_stub_230
	.quad x86_64_interrupt_stub_231
	.quad x86_64_interrupt_stub_232
	.quad x86_64_interrupt_stub_233
	.quad x86_64_interrupt_stub_234
	.quad x86_64_interrupt_stub_235
	.quad x86_64_interrupt_stub_236
	.quad x86_64_interrupt_stub_237
	.quad x86_64_interrupt_stub_238
	.quad x86_64_interrupt_stub_239
	.quad x86_64_interrupt_stub_240
	.quad x86_64_interrupt_stub_241
	.quad x86_64_interrupt_stub_242
	.quad x86_64_interrupt_stub_243
	.quad x86_64_interrupt_stub_244
	.quad x86_64_interrupt_stub_245
	.quad x86_64_interrupt_stub_246
	.quad x86_64_interrupt_stub_247
	.quad x86_64_interrupt_stub_248
	.quad x86_64_interrupt_stub_249
	.quad x86_64_interrupt_stub_250
	.quad x86_64_interrupt_stub_251
	.quad x86_64_interrupt_stub_252
	.quad x86_64_interrupt_stub_253
	.quad x86_64_interrupt_stub_254
	.quad x86_64_interrupt_stub_255

.section .note.GNU-stack,"",@progbits
