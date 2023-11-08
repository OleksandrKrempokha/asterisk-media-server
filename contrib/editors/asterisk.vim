" Vim syntax file
" Language:	Trismedia config file
" Maintainer:	tilghman
" Last Change:	2009 Mar 04 
" version 0.5
"
if version < 600
  syntax clear
elseif exists("b:current_syntax")
  finish
endif

syn sync clear
syn sync fromstart

syn keyword     trismediaTodo            TODO contained
syn match       trismediaComment         ";.*" contains=trismediaTodo
syn match       trismediaContext         "\[.\{-}\]"
syn match       trismediaExten           "^\s*exten\s*=>\?\s*[^,]\+" contains=trismediaPattern
syn match       trismediaExten           "^\s*\(register\|channel\|ignorepat\|include\|\(no\)\?load\)\s*=>\?"
syn match       trismediaPattern         "_\(\[[[:alnum:]#*\-]\+\]\|[[:alnum:]#*]\)*\.\?" contained
syn match       trismediaPattern         "[^A-Za-z0-9,]\zs[[:alnum:]#*]\+\ze" contained
syn match       trismediaApp             ",\zs[a-zA-Z]\+\ze$"
syn match       trismediaApp             ",\zs[a-zA-Z]\+\ze("
" Digits plus oldlabel (newlabel)
syn match       trismediaPriority        ",\zs[[:digit:]]\+\(+[[:alpha:]][[:alnum:]_]*\)\?\(([[:alpha:]][[:alnum:]_]*)\)\?\ze," contains=trismediaLabel
" oldlabel plus digits (newlabel)
syn match       trismediaPriority        ",\zs[[:alpha:]][[:alnum:]_]*+[[:digit:]]\+\(([[:alpha:]][[:alnum:]_]*)\)\?\ze," contains=trismediaLabel
" s or n plus digits (newlabel)
syn match       trismediaPriority        ",\zs[sn]\(+[[:digit:]]\+\)\?\(([[:alpha:]][[:alnum:]_]*)\)\?\ze," contains=trismediaLabel
syn match       trismediaLabel           "(\zs[[:alpha:]][[:alnum:]]*\ze)" contained
syn match       trismediaError           "^\s*#\s*[[:alnum:]]*"
syn match       trismediaInclude         "^\s*#\s*\(include\|exec\)\s.*"
syn region      trismediaVar             matchgroup=trismediaVarStart start="\${" end="}" contains=trismediaVar,trismediaFunction,trismediaExp
syn match       trismediaVar             "\zs[[:alpha:]][[:alnum:]_]*\ze=" contains=trismediaVar,trismediaFunction,trismediaExp
syn match       trismediaFunction        "\${_\{0,2}[[:alpha:]][[:alnum:]_]*(.*)}" contains=trismediaVar,trismediaFunction,trismediaExp
syn match       trismediaFunction        "(\zs[[:alpha:]][[:alnum:]_]*(.\{-})\ze=" contains=trismediaVar,trismediaFunction,trismediaExp
syn region      trismediaExp             matchgroup=trismediaExpStart start="\$\[" end="]" contains=trismediaVar,trismediaFunction,trismediaExp
syn match       trismediaCodecsPermit    "^\s*\(allow\|disallow\)\s*=\s*.*$" contains=trismediaCodecs
syn match       trismediaCodecs          "\(g723\|gsm\|ulaw\|alaw\|g726\|adpcm\|slin\|lpc10\|g729\|speex\|ilbc\|all\s*$\)"
syn match       trismediaError           "^\(type\|auth\|permit\|deny\|bindaddr\|host\)\s*=.*$"
syn match       trismediaType            "^\zstype=\ze\<\(peer\|user\|friend\)\>$" contains=trismediaTypeType
syn match       trismediaTypeType        "\<\(peer\|user\|friend\)\>" contained
syn match       trismediaAuth            "^\zsauth\s*=\ze\s*\<\(md5\|rsa\|plaintext\)\>$" contains=trismediaAuthType
syn match       trismediaAuthType        "\<\(md5\|rsa\|plaintext\)\>" contained
syn match       trismediaAuth            "^\zs\(secret\|inkeys\|outkey\)\s*=\ze.*$"
syn match       trismediaAuth            "^\(permit\|deny\)\s*=\s*\d\{1,3}\.\d\{1,3}\.\d\{1,3}\.\d\{1,3}/\d\{1,3}\(\.\d\{1,3}\.\d\{1,3}\.\d\{1,3}\)\?\s*$" contains=trismediaIPRange
syn match       trismediaIPRange         "\d\{1,3}\.\d\{1,3}\.\d\{1,3}\.\d\{1,3}/\d\{1,3}\.\d\{1,3}\.\d\{1,3}\.\d\{1,3}" contained
syn match       trismediaIP              "\d\{1,3}\.\d\{1,3}\.\d\{1,3}\.\d\{1,3}" contained
syn match       trismediaHostname        "\([[:alnum:]\-]*\.\)\+[[:alpha:]]\{2,10}" contained
syn match       trismediaPort            "\d\{1,5}" contained
syn match       trismediaSetting         "^\(tcp\|tls\)\?bindaddr\s*=\s*\d\{1,3}\.\d\{1,3}\.\d\{1,3}\.\d\{1,3}$" contains=trismediaIP
syn match       trismediaError           "port\s*=.*$"
syn match       trismediaSetting         "^\(bind\)\?port\s*=\s*\d\{1,5}\s*$" contains=trismediaPort
syn match       trismediaSetting         "^host\s*=\s*\(dynamic\|\(\d\{1,3}\.\d\{1,3}\.\d\{1,3}\.\d\{1,3}\)\|\([[:alnum:]\-]*\.\)\+[[:alpha:]]\{2,10}\)" contains=trismediaIP,trismediaHostname
syn match		trismediaError			"[[:space:]]$"

" Define the default highlighting.
" For version 5.7 and earlier: only when not done already
" For version 5.8 and later: only when an item doesn't have highlighting yet
if version >= 508 || !exists("did_conf_syntax_inits")
  if version < 508
    let did_conf_syntax_inits = 1
    command -nargs=+ HiLink hi link <args>
  else
    command -nargs=+ HiLink hi def link <args>
  endif

  HiLink        trismediaComment         Comment
  HiLink        trismediaExten           String
  HiLink        trismediaContext         Preproc
  HiLink        trismediaPattern         Type
  HiLink        trismediaApp             Statement
  HiLink        trismediaInclude         Preproc
  HiLink        trismediaPriority        Preproc
  HiLink        trismediaLabel           Type
  HiLink        trismediaVar             String
  HiLink        trismediaVarStart        String
  HiLink        trismediaFunction        Function
  HiLink        trismediaExp             Type
  HiLink        trismediaExpStart        Type
  HiLink        trismediaCodecsPermit    Preproc
  HiLink        trismediaCodecs          String
  HiLink        trismediaType            Statement
  HiLink        trismediaTypeType        Type
  HiLink        trismediaAuth            String
  HiLink        trismediaAuthType        Type
  HiLink        trismediaIPRange         Identifier
  HiLink        trismediaIP              Identifier
  HiLink        trismediaPort            Identifier
  HiLink        trismediaHostname        Identifier
  HiLink        trismediaSetting         Statement
  HiLink        trismediaError           Error
 delcommand HiLink
endif
let b:current_syntax = "trismedia" 
" vim: ts=8 sw=2

