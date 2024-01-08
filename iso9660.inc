;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
;;                                                              ;;
;; Copyright (C) KolibriOS team 2004-2016. All rights reserved. ;;
;;  Distributed under terms of the GNU General Public License.  ;;
;;                                                              ;;
;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;

$Revision: 7136 $

DESCRIPTOR_TYPE_BOOT          = 0x00
; для драйвера бесполезный дескриптор, скипаем
DESCRIPTOR_TYPE_PRIMARY       = 0x01
; +8 : rb 32 - strA имя системы, которая может взаимодействовать на селекторы 0-0F тома
; +40 : rb 32 - strD идентификация тома
; +80 : rb 4 - int32 количество логических блоков, в которые записан том
; +120: rb 2 - int16 размер набора в этом логическом томе (количество дисков)
; +124: rb 2 - int16 номер этого диска в наборе томов
; +128: rb 2 - int16 размер логического блока в байтах. Он не всегда 2048 может быть!
; +132: rb 4 - int32 размер таблицы путей в байтах
; +140: rb 4 - int32 LBA адрес таблицы путей
; +144: rb 4 - int32 LBA адрес необязательной таблицы путей
; +156: rb 34 - Запись корневого каталога именно запись!!! не LBA
DESCRIPTOR_TYPE_SUPPLEMENTARY = 0x02
; тоже что и TYPE_PRIMARY, но для расширения Juliet(UTF-16)
DESCRIPTOR_TYPE_PARTITION     = 0x02
DESCRIPTOR_TYPE_TERMANATOR    = 0xff
; Пустой дескриптор, который останавливает поиск

struct  ISO9660_VOLUME_DESCRIPTOR
        type    rb 1
        magic   rb 5  ; 'CD001'
        version rb 1
ends

struct  ISO9660_PRIMARY_DESCRIPTOR  ISO9660_VOLUME_DESCRIPTOR
                        rb 1
                        rb 32
        VolumeName      rb 32 ; ascii or utf-16
                        rb 4
        VolumeSpaceSize rb 8
                        rb 32
                        rb 4 ; +120
                        rb 4
        LBA_Size        rb 4 ; +128
        PathTableSize   rb 8         ; Do not use it!!!
        LBA_PathTable   rb 4 ; +140  ; Do not use it!!!
                        rb 4 ; +144  ; Do not use it!!!
                        rb 8
        root_dir_record rb 34  ;TODO
                        rb 128*4 + 37*3
        date_created    rb 17
        date_modiffed   rb 17
                        rb 17  ; протухание файла
        file_struct_ver rb 1 ;always 0x01
ends

; структура записи директории
; +0 : rb 1 - size struct
; +1 : rb 1 - увеличенная длина записи атрибута
; +2 : rb 4 - LBA
; +10 : rb 4 - size
; +18 : rb 7 - date and time
; +25 : rb 1 - flags
;               - 1b 'скрытый'
;               - 10b директория
; +32 : rb 1 - name length
; +33 : rb ? - name
struct  ISO9660_DIRECTORY_RECORD
        size            rb 1
                        rb 1
        lba             rd 1
                        rd 1
        data_length     rd 1
                        rd 1
        date_time       rb 7
        flags           rb 1
                        rb 2+4
        name_len        rb 1
        name            rb 1 ;  rb [name_len]
ends

; Internal data for every ISO9660 partition.
struct ISO9660 PARTITION
       type_encoding    rd 1 ; 0 - ascii 1 - UCS-2
       lba_size         rd 1 ; default 2048 TODO
       primary_descr    rd 1 ; sector num
       root_dir_lba     rd 1 ; lba32
       root_dir_len     rd 1 ;
ends


; FAT external functions
;   in:
; ebx -> parameter structure of sysfunc 70
; ebp -> ISO9660 structure
; esi -> path string in UTF-8
;   out:
; eax, ebx = return values for sysfunc 70
iglobal
align 4
iso9660_user_functions:
        dd      iso9660_free
        dd      (.end - $) / 4
        dd      iso9660_Read
        dd      iso9660_ReadFolder
        dd      0
        dd      0
        dd      0
        dd      iso9660_GetFileInfo
.end:
endg

; mount if it's a valid ISO9660 partition
iso9660_create_partition:
;   in:
; ebp -> PARTITION structure
; ebx -> boot sector
; ebx + 2048 -> buffer
; esi -> DISK structure
;   out:
; eax -> iso9660 partition structure, 0 = not iso9660
        cmp     dword [esi + DISK.MediaInfo.SectorSize], 2048 ; cd disks
        jnz     .fail_disk_sector

        push    ebx
        sub     esp, 4*3 ; locals value: primary_lba, encoding, select_sector
        mov     dword[esp], 16-1 ; sector number
        mov     dword[esp + 4], 0 ; base encoding - ascii

        add     ebx, 2048
.new_descr:
        inc     dword[esp]
        ; читаем 16 сектор, смотрим заголовок дескриптора
        xor     edx, edx
        mov     eax, [esp]
        mov     ecx, 1
        ; ebx - buffer
        ; edx:eax - num sector
        ; ebp - PARTITION
        ; ecx - number sectors
        call    fs_read64_sys
        test    eax, eax
        jnz     .err_disk_1

        cmp     dword[ebx + 1], 'CD00'
        jnz     .err_disk_1
        cmp     byte[ebx + 5], '1'
        jnz     .err_disk_1

        cmp     byte[ebx], DESCRIPTOR_TYPE_TERMANATOR
        jz      .end_scan

        cmp     byte[ebx], DESCRIPTOR_TYPE_PRIMARY
        jnz     @f

        cmp     dword[esp + 4], 0
        jnz     .new_descr

        mov     dword[esp + 4], 0 ; set UCS-2 encoding
        mov     eax, [esp]
        mov     dword[esp + 8], eax ; set lba address

        jmp     .new_descr
@@:
        cmp     byte[ebx], DESCRIPTOR_TYPE_SUPPLEMENTARY
        jnz     .new_descr

        mov     dword[esp + 4], 1 ; set UCS-2 encoding
        mov     eax, [esp]
        mov     dword[esp + 8], eax ; set lba address

        jmp     .new_descr
.end_scan:

        ; get root dir lba, root dir size, size lba
        xor     edx, edx
        mov     eax, [esp + 8]
        mov     ecx, 1
        call    fs_read64_sys
        test    eax, eax
        jnz     .err_disk_1

        ; alloc memory for ISO9660 struct
        mov     eax, sizeof.ISO9660
        call    malloc
        test    eax, eax
        jz      .err_disk_1
        ; copy data on struct
        add    esp, 4
        pop    dword[eax + ISO9660.type_encoding]
        mov    dword[eax + Iso9660.lba_size], 2048 ;TODO
        pop    dword[eax + ISO9660.primary_descr]

        mov     ecx, dword[ebx + ISO9660_PRIMARY_DESCRIPTOR.root_dir_record\
                               + ISO9660_DIRECTORY_RECORD.lba]
        mov     [eax + ISO9660.root_dir_lba], ecx

        mov     ecx, dword[ebx + ISO9660_PRIMARY_DESCRIPTOR.root_dir_record\
                               + ISO9660_DIRECTORY_RECORD.data_length]
        mov     [eax + ISO9660.root_dir_len], ecx

        push    edi esi
        mov     edi, eax
        mov     esi, ebp
        mov     ecx, sizeof.PARTITION/4
        rep movsd
        pop     esi edi

        pop     ebx
        ret
.err_disk_1:
        add     esp, 4*3
        pop     ebx
.fail_disk_sector:
        xor     eax, eax
        ret

; IN: eax - ptr PARTITION
; OUT: -
; SAVE: esi, edi
;  Function free PARTITION struct and all object this structure
iso9660_free:
        jmp     free




; ISO9660 external functions
;   in:
; ebx -> parameter structure of sysfunc 70
; ebp -> ISO9660 structure
; esi -> path string in UTF-8
;   out:
; eax, ebx = return values for sysfunc 70
iso9660_Read:
        sub     esp, 4 ; for ptr on memory page
        call    iso9660_find_file
        mov     esi, eax
        mov     edi, [ebx + 16] ; ptr to programm buffer

        test    byte[esi + ISO9660_DIRECTORY_RECORD.flags], 10b ; check dir
        jnz     iso9660_find_file.not_found

        ; check offset (offset <= size)
        mov     eax, [ebx + 4]  ; low offset
        mov     edx, eax
        cmp     [ebx + 8], 0  ; high offset
        jnz     iso9660_find_file.bad_offset ; error offset > max size

        add     eax, [ebx + 12] ;size copy data
        jc      iso9660_find_file.bad_offset ; error offset + size > max size

        cmp     edx, [esi + ISO9660_DIRECTORY_RECORD.data_length]
        jae     iso9660_find_file.bad_offset ; error offset > file length

        ; good file - copy file data
        sub     esp, 4*4            ; esp+3*4 = fs err code  esp+2*4= size copy data
        mov     dword[esp + 3*4], 0 ; esp+1*4 = offset copy  esp = count copying data

        mov     [esp + 2*4], eax ; copy finish offset
        sub     [esp + 2*4], edx ; = size copy data
        mov     [esp + 1*4], edx ; offset to start copy
        mov     [esp], 0 ; count copping byte

        ; check end offset (offset+size_buff <= size)
        cmp     eax, [esi + ISO9660_DIRECTORY_RECORD.data_length]
        jbe      .full_size ;  offset+size_buff <= file length

        ; read offset - (file size - offset)
        mov     dword[esp + 3*4], 6
        mov     eax, [esi + ISO9660_DIRECTORY_RECORD.data_length]
        sub     eax, edx ;[esp + 1*4]
        mov     dword[esp + 2*4], eax ; = size copy data

.full_size:
        ; check offset mod sector_size = 0
        test    edx, not -2048
        jz      .first_align ; no creat buffer for first align

        push    edx
        stdcall kernel_alloc, 4096  ; TODO
        mov     ebx, eax
        pop     edx

        test    eax, eax
        jz      .err_memory

        ; read sector
        push    edx ebx

        and     edx, -2048
        shr     edx, BSF 2048
        mov     eax, [esi + ISO9660_DIRECTORY_RECORD.lba]
        add     eax, edx
        mov     ecx, 1
        xor     edx, edx
        ; ebx - buffer
        ; edx:eax - num sector
        ; ebp - PARTITION
        ; ecx - number sectors
        call    fs_read64_sys

        pop     ebx edx
        test    eax, eax
        jnz     .err_disk_1

        not     edx
        and     edx, not -2048

        add     dword[esp + 1*4], not -2048
        and     dword[esp + 1*4], -2048
        cmp     dword[esp + 2*4], edx
        jb      @f
        mov     edx, [esp + 2*4]
@@:
        sub     dword[esp + 2*4], edx
        add     dword[esp], edx

        push    esi
        ; copy (2048 - offset) and -2048
        mov     esi, ebx ; buffer
        mov     ecx, edx
        rep movsb
        pop    esi

        stdcall kernel_free, ebx
.first_align:

        mov     ecx, [esp + 2*4]
        and     ecx, -2048

        cmp     ecx, 2048
        jb      .copy_finish_block

        mov     eax, [esp + 1*4]
        shr     eax, BSF 2048
        ; copy main block
        mov     ebx, edi
        add     edi, ecx
        sub     dword[esp + 2*4], ecx
        add     dword[esp + 1*4], ecx
        add     dword[esp], ecx

        shr     ecx, BSF 2048
        xor     edx, edx
        add     eax, [esi + ISO9660_DIRECTORY_RECORD.lba]
        ; ebx - buffer
        ; edx:eax - num sector
        ; ebp - PARTITION
        ; ecx - number sectors
        call    fs_read64_sys
        test    eax, eax
        jnz     .err_disk

.copy_finish_block:

        cmp     dword[esp + 2*4], 0
        jz      .end_align ; creat buffer for end read sector

        stdcall kernel_alloc, 4096
        mov     ebx, eax
        jz      .err_memory

        ;copy finish block
        mov     eax, [esp + 1*4]
        shr     eax, BSF 2048
        xor     edx, edx

        mov     ecx, 1
        add     eax, [esi + ISO9660_DIRECTORY_RECORD.lba]
        ; ebx - buffer
        ; edx:eax - num sector
        ; ebp - PARTITION
        ; ecx - number sectors
        call    fs_read64_sys
        test    eax, eax
        jnz     .err_disk_1

        mov     esi, ebx
        mov     ecx, [esp + 2*4]
        add     dword[esp], ecx
        rep movsb

        stdcall kernel_free, ebx

.end_align:
        ; set ebx size copy data
        mov     ebx, [esp]
        mov     esi, [esp + 3*4]
        add     esp, 4*4
        call    kernel_free

        mov     eax, esi
        ret

.err_disk_1:
        stdcall kernel_free, ebx
.err_disk:
        add     esp, 4*4
        call    kernel_free
        xor     ebx, ebx
        mov     eax, 11 ; device error
        ret
.err_memory:
        add     esp, 4*4
        call    kernel_free
        xor     ebx, ebx
        mov     eax, 12
        ret


; FAT external functions
;   in:
; ebx -> parameter structure of sysfunc 70
; ebp -> ISO9660 structure
; esi -> path string in UTF-8
;   out:
; eax, ebx = return values for sysfunc 70
iso9660_ReadFolder:
        sub     esp, 4 ; for ptr on memory page
        call    iso9660_find_file

        test    byte[eax + ISO9660_DIRECTORY_RECORD.flags], 10b ; check dir
        jz      iso9660_find_file.not_found

        ; create buffer for read sectors

        ; check index file in dir

        ; loop copy file info and name
.new_file:

        call    iso9660_GetFileInfo.copy_file_info

        ; copy encoding

        ; copy name

        ; free buffer

        ret

; ISO9660 external functions
;   in:
; ebx -> parameter structure of sysfunc 70
; ebp -> ISO9660 structure
; esi -> path string in UTF-8
;   out:
; eax, ebx = return values for sysfunc 70
iso9660_GetFileInfo:
        cmp     byte[esi], 0
        jz      .rootdir

        sub     esp, 4 ; for ptr on memory page
        call    iso9660_find_file

        mov     ecx, [ebx + 16] ; buffer

        call    .copy_file_info

        call    kernel_free
        xor     eax, eax
        mov     ebx, 40
        ret

;  IN: eax -> ISO966_DIRECTORY_RECORD
;      ecx -> buffer
; destruct: edx
.copy_file_info:
        ; copy flags(dir of file)
        xor     edx, edx
        or      dl, 100001b
        test    byte[eax + ISO9660_DIRECTORY_RECORD.flags], 1b ; check hidden flag
        jz      @f
        or      dl, 10b
@@:
        test    byte[eax + ISO9660_DIRECTORY_RECORD.flags], 10b ; check dir
        jz      @f
        and     dl, not 1b
        or      dl, 10000b ; dir flag
@@:
        mov     [ecx], edx
        ; copy size
        mov     [ecx + 36], 0
        mov     edx, [eax + ISO9660_DIRECTOEY_RECORD.data_length]
        mov     [ecx + 32], edx

        ; copy date creat file
        movzx   edx, byte[eax + ISO9660_DIRECTOEY_RECORD.date_time]
        add     edx, 1900 ; year
        shl     edx, 8

        mov     dl, byte[eax + ISO9660_DIRECTOEY_RECORD.date_time + 1] ;month
        shl     edx, 8
        mov     dl, byte[eax + ISO9660_DIRECTOEY_RECORD.date_time + 2] ;day

        mov     [ecx + 12], edx
        mov     [ecx + 20], edx
        mov     [ecx + 28], edx

        ; copy time creat file
        movzx   edx, byte[eax + ISO9660_DIRECTOEY_RECORD.date_time + 3] ;hour
        shl     edx, 8
        mov     dl, byte[eax + ISO9660_DIRECTOEY_RECORD.date_time + 4] ; minute
        shl     edx, 8
        mov     dl, byte[eax + ISO9660_DIRECTOEY_RECORD.date_time + 5] ; second

        mov     [ecx + 8], edx
        mov     [ecx + 16], edx
        mov     [ecx + 24], edx

        ret

.rootdir:
        mov     edi, [ebx + 16] ; edi = buffer
        ; copy flags (dir)
        mov     byte [edi], 8
        ; copy size drive
        mov     eax, dword[ebp + PARTITION.Length+DQ.lo]
        mov     edx, dword[ebp + PARTITION.Length+DQ.hi]
        mov     ecx, [ebp + PARTITION.Disk]
        mov     ecx, [ecx + DISK.MediaInfo.SectorSize]
        bsf     ecx, ecx
        shld    edx, eax, cl
        shl     eax, cl
        mov     [edi + 32], eax  ; bdfe.size.lo
        mov     [edi + 36], edx  ; bdfe.size.hi

        mov     eax, [ebx + 8]
        ; copy encoding
        mov     [edi + 4], eax
        ; check encoding on fs struct
        test    eax, eax ; check f70s5arg.xflags
        jz      .no_name

        ; stdcall is_region_userspace, edi, ecx

        ; alloc memory for read primary descriptor
        stdcall kernel_alloc, PAGE_SIZE
        test    eax, eax
        jz      .no memory

        push    eax
        mov     esi, eax
        ; read primary descriptor
        mov     ebx, eax
        mov     ecx, 1
        xor     edx, edx
        mov     eax, [ebp + ISO9660.primary_descr]
        ; ebx - buffer
        ; edx:eax - num sector
        ; ebp - PARTITION
        ; ecx - number sectors
        call    fs_read64_sys
        test    eax, eax
        jnz     .err_read_part

        add     esi, ISO9660_PRIMARY_DESCRIPTOR.VolumeName
        mov     edx, [edi + 4]
        add     edi, 40 ; offset partition name
        mov     ecx, 32
        call    iso9660_copy_name

        ; free memory
        call    kernel_free
.no_name:
        xor     eax, eax
        mov     ebx, 40
        ret
.err_read_part:
        call    kernel_free

        mov     eax, 11
        ret
.no_memory:
        mov     eax, 12
        ret


;; private function

; IN: esi - ptr string UTF-8 or zero for root directory
; OUT: eax - ptr to directory record
;      [esp + 4] - ptr to memory page for destruct
iso9660_find_file:
        cmp     byte[esi], 0
        jz      .rootdir


.next_dir:
.next_sector:
        ;get sector directory
        ; find name

.found:
        ; finish name?
        ; no: jmp .next_dir

        ; malloc memory for dir record
        ; copy data
        ret
.rootdir:
        ;get primary discriptor
        ; malloc memory for dir record
        ; copy data
        ret
; errors
.not_found_1:
        add     esp, 4
.not_found: ; esp - ptr to page
        call    kernel_free

        mov     eax, 5
        xor     ebx, ebx
        ret

.bad_offset: ; esp - ptr to page
        call    kernel_free

        mov     eax, 6
        xor     ebx, ebx
        ret


.no_memor_1:
        add     esp, 4
.no_memory:
        mov     eax, 12
        xor     ebx, ebx
        ret



;IN: ebp -> PARTITION structure
;    ebx -> FS structure
;    esi -> input string (ascii or ucs2)
;    ecx =  count bytes
;    edi -> output string (buffer)
;    edx =  encoding output string
;OUT: edi increasing
;     esi increasing
;     ecx = 0 or other value
iso9660_copy_name:
        cmp     [ebp + ISO9660.type_encoding], 0
        jnz     .ucs2

        cmp     edx, 1 ; cp866 encoding
        jne     @f
        ; no convert chars
        rep movsb
        jmp     .no_name
@@:
        cmp     edx, 2 ; utf16 encoding
        jne     .ascii2utf8
        ; convert ascii to utf16LE
@@:
        lodsb
        call    ansi2uni_char
        stosw
        loop    @b
        jmp     .end_copy_name

.ascii2utf8:
        cmp     edx, 3 ; utf8 encoding
        jne     .end_copy_name
        ; convert ascii to utf8
        call    cp866toUTF8_string
        jmp     .end_copy_name
.ucs2:
        shr     ecx, 1 ; ecx/2 - ucs2 chars

        cmp     edx, 1 ; cp866 encoding
        jne     .ucs2utf16
        ; convert ucs2 to ascii
@@:
        lodsw
        xchg    al, ah
        call    uni2ansi_char
        stosb
        loop    @b

        jmp     .end_copy_name
.ucs2utf16:
        cmp     edx, 2 ; utf16 encoding
        jne     .ucs2utf8
        ; convert ucs2 to utf16LE
@@:
        lodsw
        xchg    al, ah
        stosw
        loop    @b
        jmp     .end_copy_name

.ucs2utf8:
        cmp     edx, 3 ; utf8 encoding
        jne     .end_copy_name
        ; convert ucs2 to utf8
        mov     ebx, ecx
        shl     ecx, 1
@@:
        lodsw
        xchg    ah, al
        call    UTF16to8
        dec     ebx
        jnz     @b

.end_copy_name:

        ret








