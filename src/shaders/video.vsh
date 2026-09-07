; Vertex shader for the video quad.
;
; Positions arrive already in clip space and texture coordinates already
; normalised, both computed on the CPU once per frame, so there is nothing to
; transform here: this passes them through.
;
; Attribute location 0 lands in R1, location 1 in R2.

; $MODE = "UniformRegister"

; $ATTRIB_VARS[0].name = "a_position"
; $ATTRIB_VARS[0].type = "vec2"
; $ATTRIB_VARS[0].location = 0
; $ATTRIB_VARS[1].name = "a_texCoord"
; $ATTRIB_VARS[1].type = "vec2"
; $ATTRIB_VARS[1].location = 1

; $SPI_VS_OUT_CONFIG.VS_EXPORT_COUNT = 0
; $NUM_SPI_VS_OUT_ID = 1
; $SPI_VS_OUT_ID[0].semantic_0 = 0

00 CALL_FS NO_BARRIER
01 ALU: ADDR(32) CNT(3)
      0  z: MOV R1.z, 0.0f
         w: MOV R1.w, (0x3F800000, 1.0f).x
02 EXP_DONE: POS0, R1
03 EXP_DONE: PARAM0, R2.xyzz NO_BARRIER
END_OF_PROGRAM
