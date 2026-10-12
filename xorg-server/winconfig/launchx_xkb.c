#include "launchx_xkb.h"

struct xkb_variant {
    const char *layout;
    const char *code;
    const char *name;
};

static const struct xkb_variant variants[] = {
    { "al", "plisi", "plisi                       Albanian (Plisi)" },
    { "al", "veqilharxhi", "veqilharxhi                 Albanian (Veqilharxhi)" },
    { "am", "phonetic", "phonetic                    Armenian (phonetic)" },
    { "am", "phonetic-alt", "phonetic-alt                Armenian (alt. phonetic)" },
    { "am", "eastern", "eastern                     Armenian (eastern)" },
    { "am", "eastern-alt", "eastern-alt                 Armenian (alt. eastern)" },
    { "am", "western", "western                     Armenian (western)" },
    { "ara", "digits", "digits                      Arabic (Eastern Arabic numerals)" },
    { "ara", "azerty", "azerty                      Arabic (AZERTY)" },
    { "ara", "azerty_digits", "azerty_digits               Arabic (AZERTY, Eastern Arabic numerals)" },
    { "ara", "buckwalter", "buckwalter                  Arabic (Buckwalter)" },
    { "ara", "mac", "mac                         Arabic (Macintosh)" },
    { "ara", "mac-phonetic", "mac-phonetic                Arabic (Macintosh, phonetic)" },
    { "ara", "olpc", "olpc                        Arabic (OLPC)" },
    { "iq", "ku", "ku                          Kurdish (Iraq, Latin Q)" },
    { "iq", "ku_alt", "ku_alt                      Kurdish (Iraq, Latin Alt-Q)" },
    { "iq", "ku_f", "ku_f                        Kurdish (Iraq, F)" },
    { "iq", "ku_ara", "ku_ara                      Kurdish (Iraq, Arabic-Latin)" },
    { "ma", "tifinagh", "tifinagh                    Berber (Morocco, Tifinagh)" },
    { "ma", "tifinagh-alt", "tifinagh-alt                Berber (Morocco, Tifinagh alt.)" },
    { "ma", "tifinagh-alt-phonetic", "tifinagh-alt-phonetic       Berber (Morocco, Tifinagh phonetic, alt.)" },
    { "ma", "tifinagh-extended", "tifinagh-extended           Berber (Morocco, Tifinagh extended)" },
    { "ma", "tifinagh-phonetic", "tifinagh-phonetic           Berber (Morocco, Tifinagh phonetic)" },
    { "ma", "tifinagh-extended-phonetic", "tifinagh-extended-phonetic  Berber (Morocco, Tifinagh extended phonetic)" },
    { "ma", "french", "french                      French (Morocco)" },
    { "ma", "rif", "rif                         Tarifit" },
    { "sy", "syc", "syc                         Syriac" },
    { "sy", "syc_phonetic", "syc_phonetic                Syriac (phonetic)" },
    { "sy", "ku", "ku                          Kurdish (Syria, Latin Q)" },
    { "sy", "ku_alt", "ku_alt                      Kurdish (Syria, Latin Alt-Q)" },
    { "sy", "ku_f", "ku_f                        Kurdish (Syria, F)" },
    { "az", "cyrillic", "cyrillic                    Azerbaijani (Cyrillic)" },
    { "ml", "fr-oss", "fr-oss                      French (Mali, alt.)" },
    { "ml", "us-mac", "us-mac                      English (Mali, US, Macintosh)" },
    { "ml", "us-intl", "us-intl                     English (Mali, US, intl.)" },
    { "bd", "probhat", "probhat                     Bangla (Probhat)" },
    { "by", "legacy", "legacy                      Belarusian (legacy)" },
    { "by", "latin", "latin                       Belarusian (Latin)" },
    { "by", "intl", "intl                        Belarusian (intl.)" },
    { "by", "phonetic", "phonetic                    Belarusian (phonetic)" },
    { "by", "ru", "ru                          Russian (Belarus)" },
    { "be", "oss", "oss                         Belgian (alt.)" },
    { "be", "oss_latin9", "oss_latin9                  Belgian (Latin-9 only, alt.)" },
    { "be", "iso-alternate", "iso-alternate               Belgian (ISO, alt.)" },
    { "be", "nodeadkeys", "nodeadkeys                  Belgian (no dead keys)" },
    { "be", "wang", "wang                        Belgian (Wang 724 AZERTY)" },
    { "dz", "ber", "ber                         Berber (Algeria, Tifinagh)" },
    { "dz", "azerty-deadkeys", "azerty-deadkeys             Kabyle (AZERTY, with dead keys)" },
    { "dz", "qwerty-gb-deadkeys", "qwerty-gb-deadkeys          Kabyle (QWERTY, UK, with dead keys)" },
    { "dz", "qwerty-us-deadkeys", "qwerty-us-deadkeys          Kabyle (QWERTY, US, with dead keys)" },
    { "dz", "ar", "ar                          Arabic (Algeria)" },
    { "ba", "alternatequotes", "alternatequotes             Bosnian (with guillemets)" },
    { "ba", "unicode", "unicode                     Bosnian (with Bosnian digraphs)" },
    { "ba", "unicodeus", "unicodeus                   Bosnian (US, with Bosnian digraphs)" },
    { "ba", "us", "us                          Bosnian (US)" },
    { "brai", "left_hand", "left_hand                   Braille (one-handed, left)" },
    { "brai", "left_hand_invert", "left_hand_invert            Braille (one-handed, left, inverted thumb)" },
    { "brai", "right_hand", "right_hand                  Braille (one-handed, right)" },
    { "brai", "right_hand_invert", "right_hand_invert           Braille (one-handed, right, inverted thumb)" },
    { "bg", "phonetic", "phonetic                    Bulgarian (traditional phonetic)" },
    { "bg", "bas_phonetic", "bas_phonetic                Bulgarian (new phonetic)" },
    { "bg", "bekl", "bekl                        Bulgarian (enhanced)" },
    { "mm", "zawgyi", "zawgyi                      Burmese (Zawgyi)" },
    { "mm", "mnw", "mnw                         Mon" },
    { "mm", "mnw-a1", "mnw-a1                      Mon (A1)" },
    { "mm", "shn", "shn                         Shan" },
    { "mm", "zgt", "zgt                         Shan (Zawgyi)" },
    { "cn", "altgr-pinyin", "altgr-pinyin                Hanyu Pinyin Letters (with AltGr dead keys)" },
    { "cn", "mon_trad", "mon_trad                    Mongolian (Bichig)" },
    { "cn", "mon_trad_todo", "mon_trad_todo               Mongolian (Todo)" },
    { "cn", "mon_trad_xibe", "mon_trad_xibe               Mongolian (Xibe)" },
    { "cn", "mon_trad_manchu", "mon_trad_manchu             Mongolian (Manchu)" },
    { "cn", "mon_trad_galik", "mon_trad_galik              Mongolian (Galik)" },
    { "cn", "mon_todo_galik", "mon_todo_galik              Mongolian (Todo Galik)" },
    { "cn", "mon_manchu_galik", "mon_manchu_galik            Mongolian (Manchu Galik)" },
    { "cn", "tib", "tib                         Tibetan" },
    { "cn", "tib_asciinum", "tib_asciinum                Tibetan (with ASCII numerals)" },
    { "cn", "ug", "ug                          Uyghur" },
    { "hr", "alternatequotes", "alternatequotes             Croatian (with guillemets)" },
    { "hr", "unicode", "unicode                     Croatian (with Croatian digraphs)" },
    { "hr", "unicodeus", "unicodeus                   Croatian (US, with Croatian digraphs)" },
    { "hr", "us", "us                          Croatian (US)" },
    { "cz", "bksl", "bksl                        Czech (extra backslash)" },
    { "cz", "qwerty", "qwerty                      Czech (QWERTY)" },
    { "cz", "qwerty_bksl", "qwerty_bksl                 Czech (QWERTY, extra backslash)" },
    { "cz", "winkeys", "winkeys                     Czech (QWERTZ, Windows)" },
    { "cz", "winkeys-qwerty", "winkeys-qwerty              Czech (QWERTY, Windows)" },
    { "cz", "qwerty-mac", "qwerty-mac                  Czech (QWERTY, Macintosh)" },
    { "cz", "ucw", "ucw                         Czech (UCW, only accented letters)" },
    { "cz", "dvorak-ucw", "dvorak-ucw                  Czech (US, Dvorak, UCW support)" },
    { "cz", "rus", "rus                         Russian (Czechia, phonetic)" },
    { "dk", "nodeadkeys", "nodeadkeys                  Danish (no dead keys)" },
    { "dk", "winkeys", "winkeys                     Danish (Windows)" },
    { "dk", "mac", "mac                         Danish (Macintosh)" },
    { "dk", "mac_nodeadkeys", "mac_nodeadkeys              Danish (Macintosh, no dead keys)" },
    { "dk", "dvorak", "dvorak                      Danish (Dvorak)" },
    { "af", "ps", "ps                          Pashto" },
    { "af", "uz", "uz                          Uzbek (Afghanistan)" },
    { "af", "fa-olpc", "fa-olpc                     Dari (Afghanistan, OLPC)" },
    { "af", "ps-olpc", "ps-olpc                     Pashto (Afghanistan, OLPC)" },
    { "af", "uz-olpc", "uz-olpc                     Uzbek (Afghanistan, OLPC)" },
    { "nl", "us", "us                          Dutch (US)" },
    { "nl", "mac", "mac                         Dutch (Macintosh)" },
    { "nl", "std", "std                         Dutch (standard)" },
    { "cm", "french", "french                      French (Cameroon)" },
    { "cm", "qwerty", "qwerty                      Cameroon Multilingual (QWERTY, intl.)" },
    { "cm", "azerty", "azerty                      Cameroon (AZERTY, intl.)" },
    { "cm", "dvorak", "dvorak                      Cameroon (Dvorak, intl.)" },
    { "cm", "mmuock", "mmuock                      Mmuock" },
    { "gh", "generic", "generic                     English (Ghana, multilingual)" },
    { "gh", "gillbt", "gillbt                      English (Ghana, GILLBT)" },
    { "gh", "akan", "akan                        Akan" },
    { "gh", "avn", "avn                         Avatime" },
    { "gh", "ewe", "ewe                         Ewe" },
    { "gh", "fula", "fula                        Fula" },
    { "gh", "ga", "ga                          Ga" },
    { "gh", "hausa", "hausa                       Hausa (Ghana)" },
    { "nz", "mao", "mao                         Maori" },
    { "ng", "hausa", "hausa                       Hausa (Nigeria)" },
    { "ng", "igbo", "igbo                        Igbo" },
    { "ng", "yoruba", "yoruba                      Yoruba" },
    { "gb", "extd", "extd                        English (UK, extended, Windows)" },
    { "gb", "intl", "intl                        English (UK, intl., with dead keys)" },
    { "gb", "dvorak", "dvorak                      English (UK, Dvorak)" },
    { "gb", "dvorakukp", "dvorakukp                   English (UK, Dvorak, with UK punctuation)" },
    { "gb", "mac", "mac                         English (UK, Macintosh)" },
    { "gb", "mac_intl", "mac_intl                    English (UK, Macintosh, intl.)" },
    { "gb", "colemak", "colemak                     English (UK, Colemak)" },
    { "gb", "colemak_dh", "colemak_dh                  English (UK, Colemak-DH)" },
    { "gb", "gla", "gla                         Scottish Gaelic" },
    { "gb", "pl", "pl                          Polish (British keyboard)" },
    { "us", "euro", "euro                        English (US, euro on 5)" },
    { "us", "intl", "intl                        English (US, intl., with dead keys)" },
    { "us", "alt-intl", "alt-intl                    English (US, alt. intl.)" },
    { "us", "altgr-intl", "altgr-intl                  English (intl., with AltGr dead keys)" },
    { "us", "mac", "mac                         English (Macintosh)" },
    { "us", "colemak", "colemak                     English (Colemak)" },
    { "us", "colemak_dh", "colemak_dh                  English (Colemak-DH)" },
    { "us", "colemak_dh_wide", "colemak_dh_wide             English (Colemak-DH Wide)" },
    { "us", "colemak_dh_ortho", "colemak_dh_ortho            English (Colemak-DH Ortholinear)" },
    { "us", "colemak_dh_iso", "colemak_dh_iso              English (Colemak-DH ISO)" },
    { "us", "colemak_dh_wide_iso", "colemak_dh_wide_iso         English (Colemak-DH Wide ISO)" },
    { "us", "dvorak", "dvorak                      English (Dvorak)" },
    { "us", "dvorak-intl", "dvorak-intl                 English (Dvorak, intl., with dead keys)" },
    { "us", "dvorak-alt-intl", "dvorak-alt-intl             English (Dvorak, alt. intl.)" },
    { "us", "dvorak-l", "dvorak-l                    English (Dvorak, one-handed, left)" },
    { "us", "dvorak-r", "dvorak-r                    English (Dvorak, one-handed, right)" },
    { "us", "dvorak-classic", "dvorak-classic              English (classic Dvorak)" },
    { "us", "dvp", "dvp                         English (programmer Dvorak)" },
    { "us", "dvorak-mac", "dvorak-mac                  English (Dvorak, Macintosh)" },
    { "us", "norman", "norman                      English (Norman)" },
    { "us", "symbolic", "symbolic                    English (US, Symbolic)" },
    { "us", "workman", "workman                     English (Workman)" },
    { "us", "workman-intl", "workman-intl                English (Workman, intl., with dead keys)" },
    { "us", "olpc2", "olpc2                       English (the divide/multiply toggle the layout)" },
    { "us", "chr", "chr                         Cherokee" },
    { "us", "haw", "haw                         Hawaiian" },
    { "us", "rus", "rus                         Russian (US, phonetic)" },
    { "us", "hbs", "hbs                         Serbo-Croatian (US)" },
    { "epo", "legacy", "legacy                      Esperanto (legacy)" },
    { "ee", "nodeadkeys", "nodeadkeys                  Estonian (no dead keys)" },
    { "ee", "dvorak", "dvorak                      Estonian (Dvorak)" },
    { "ee", "us", "us                          Estonian (US)" },
    { "fo", "nodeadkeys", "nodeadkeys                  Faroese (no dead keys)" },
    { "ph", "qwerty-bay", "qwerty-bay                  Filipino (QWERTY, Baybayin)" },
    { "ph", "capewell-dvorak", "capewell-dvorak             Filipino (Capewell-Dvorak, Latin)" },
    { "ph", "capewell-dvorak-bay", "capewell-dvorak-bay         Filipino (Capewell-Dvorak, Baybayin)" },
    { "ph", "capewell-qwerf2k6", "capewell-qwerf2k6           Filipino (Capewell-QWERF 2006, Latin)" },
    { "ph", "capewell-qwerf2k6-bay", "capewell-qwerf2k6-bay       Filipino (Capewell-QWERF 2006, Baybayin)" },
    { "ph", "colemak", "colemak                     Filipino (Colemak, Latin)" },
    { "ph", "colemak-bay", "colemak-bay                 Filipino (Colemak, Baybayin)" },
    { "ph", "dvorak", "dvorak                      Filipino (Dvorak, Latin)" },
    { "ph", "dvorak-bay", "dvorak-bay                  Filipino (Dvorak, Baybayin)" },
    { "fi", "winkeys", "winkeys                     Finnish (Windows)" },
    { "fi", "classic", "classic                     Finnish (classic)" },
    { "fi", "nodeadkeys", "nodeadkeys                  Finnish (classic, no dead keys)" },
    { "fi", "mac", "mac                         Finnish (Macintosh)" },
    { "fi", "smi", "smi                         Northern Saami (Finland)" },
    { "fr", "nodeadkeys", "nodeadkeys                  French (no dead keys)" },
    { "fr", "oss", "oss                         French (alt.)" },
    { "fr", "oss_nodeadkeys", "oss_nodeadkeys              French (alt., no dead keys)" },
    { "fr", "oss_latin9", "oss_latin9                  French (alt., Latin-9 only)" },
    { "fr", "latin9", "latin9                      French (legacy, alt.)" },
    { "fr", "latin9_nodeadkeys", "latin9_nodeadkeys           French (legacy, alt., no dead keys)" },
    { "fr", "azerty", "azerty                      French (AZERTY)" },
    { "fr", "afnor", "afnor                       French (AZERTY, AFNOR)" },
    { "fr", "bepo", "bepo                        French (BEPO)" },
    { "fr", "bepo_latin9", "bepo_latin9                 French (BEPO, Latin-9 only)" },
    { "fr", "bepo_afnor", "bepo_afnor                  French (BEPO, AFNOR)" },
    { "fr", "dvorak", "dvorak                      French (Dvorak)" },
    { "fr", "ergol", "ergol                       French (Ergo-L)" },
    { "fr", "ergol_iso", "ergol_iso                   French (Ergo-L, ISO variant)" },
    { "fr", "mac", "mac                         French (Macintosh)" },
    { "fr", "us", "us                          French (US)" },
    { "fr", "bre", "bre                         Breton (France)" },
    { "fr", "oci", "oci                         Occitan" },
    { "fr", "geo", "geo                         Georgian (France, AZERTY Tskapo)" },
    { "ca", "fr-dvorak", "fr-dvorak                   French (Canada, Dvorak)" },
    { "ca", "fr-legacy", "fr-legacy                   French (Canada, legacy)" },
    { "ca", "multix", "multix                      Canadian (CSA)" },
    { "ca", "eng", "eng                         English (Canada)" },
    { "ca", "ike", "ike                         Inuktitut" },
    { "ge", "ergonomic", "ergonomic                   Georgian (ergonomic)" },
    { "ge", "mess", "mess                        Georgian (MESS)" },
    { "ge", "os", "os                          Ossetian (Georgia)" },
    { "ge", "ru", "ru                          Russian (Georgia)" },
    { "de", "deadacute", "deadacute                   German (dead acute)" },
    { "de", "deadgraveacute", "deadgraveacute              German (dead grave acute)" },
    { "de", "deadtilde", "deadtilde                   German (dead tilde)" },
    { "de", "nodeadkeys", "nodeadkeys                  German (no dead keys)" },
    { "de", "e1", "e1                          German (E1)" },
    { "de", "e2", "e2                          German (E2)" },
    { "de", "T3", "T3                          German (T3)" },
    { "de", "us", "us                          German (US)" },
    { "de", "dvorak", "dvorak                      German (Dvorak)" },
    { "de", "mac", "mac                         German (Macintosh)" },
    { "de", "mac_nodeadkeys", "mac_nodeadkeys              German (Macintosh, no dead keys)" },
    { "de", "neo", "neo                         German (Neo 2)" },
    { "de", "qwerty", "qwerty                      German (QWERTY)" },
    { "de", "dsb", "dsb                         Lower Sorbian" },
    { "de", "dsb_qwertz", "dsb_qwertz                  Lower Sorbian (QWERTZ)" },
    { "de", "ro", "ro                          Romanian (Germany)" },
    { "de", "ro_nodeadkeys", "ro_nodeadkeys               Romanian (Germany, no dead keys)" },
    { "de", "ru", "ru                          Russian (Germany, phonetic)" },
    { "de", "tr", "tr                          Turkish (Germany)" },
    { "at", "nodeadkeys", "nodeadkeys                  German (Austria, no dead keys)" },
    { "at", "mac", "mac                         German (Austria, Macintosh)" },
    { "ch", "de_nodeadkeys", "de_nodeadkeys               German (Switzerland, no dead keys)" },
    { "ch", "de_mac", "de_mac                      German (Switzerland, Macintosh)" },
    { "ch", "legacy", "legacy                      German (Switzerland, legacy)" },
    { "ch", "fr", "fr                          French (Switzerland)" },
    { "ch", "fr_nodeadkeys", "fr_nodeadkeys               French (Switzerland, no dead keys)" },
    { "ch", "fr_mac", "fr_mac                      French (Switzerland, Macintosh)" },
    { "gr", "simple", "simple                      Greek (simple)" },
    { "gr", "nodeadkeys", "nodeadkeys                  Greek (no dead keys)" },
    { "gr", "polytonic", "polytonic                   Greek (polytonic)" },
    { "il", "si2", "si2                         Hebrew (SI-1452-2)" },
    { "il", "lyx", "lyx                         Hebrew (lyx)" },
    { "il", "phonetic", "phonetic                    Hebrew (phonetic)" },
    { "il", "biblical", "biblical                    Hebrew (Biblical, Tiro)" },
    { "hu", "standard", "standard                    Hungarian (standard)" },
    { "hu", "nodeadkeys", "nodeadkeys                  Hungarian (no dead keys)" },
    { "hu", "qwerty", "qwerty                      Hungarian (QWERTY)" },
    { "hu", "101_qwertz_comma_dead", "101_qwertz_comma_dead       Hungarian (QWERTZ, 101-key, comma, dead keys)" },
    { "hu", "101_qwertz_comma_nodead", "101_qwertz_comma_nodead     Hungarian (QWERTZ, 101-key, comma, no dead keys)" },
    { "hu", "101_qwertz_dot_dead", "101_qwertz_dot_dead         Hungarian (QWERTZ, 101-key, dot, dead keys)" },
    { "hu", "101_qwertz_dot_nodead", "101_qwertz_dot_nodead       Hungarian (QWERTZ, 101-key, dot, no dead keys)" },
    { "hu", "101_qwerty_comma_dead", "101_qwerty_comma_dead       Hungarian (QWERTY, 101-key, comma, dead keys)" },
    { "hu", "101_qwerty_comma_nodead", "101_qwerty_comma_nodead     Hungarian (QWERTY, 101-key, comma, no dead keys)" },
    { "hu", "101_qwerty_dot_dead", "101_qwerty_dot_dead         Hungarian (QWERTY, 101-key, dot, dead keys)" },
    { "hu", "101_qwerty_dot_nodead", "101_qwerty_dot_nodead       Hungarian (QWERTY, 101-key, dot, no dead keys)" },
    { "hu", "102_qwertz_comma_dead", "102_qwertz_comma_dead       Hungarian (QWERTZ, 102-key, comma, dead keys)" },
    { "hu", "102_qwertz_comma_nodead", "102_qwertz_comma_nodead     Hungarian (QWERTZ, 102-key, comma, no dead keys)" },
    { "hu", "102_qwertz_dot_dead", "102_qwertz_dot_dead         Hungarian (QWERTZ, 102-key, dot, dead keys)" },
    { "hu", "102_qwertz_dot_nodead", "102_qwertz_dot_nodead       Hungarian (QWERTZ, 102-key, dot, no dead keys)" },
    { "hu", "102_qwerty_comma_dead", "102_qwerty_comma_dead       Hungarian (QWERTY, 102-key, comma, dead keys)" },
    { "hu", "102_qwerty_comma_nodead", "102_qwerty_comma_nodead     Hungarian (QWERTY, 102-key, comma, no dead keys)" },
    { "hu", "102_qwerty_dot_dead", "102_qwerty_dot_dead         Hungarian (QWERTY, 102-key, dot, dead keys)" },
    { "hu", "102_qwerty_dot_nodead", "102_qwerty_dot_nodead       Hungarian (QWERTY, 102-key, dot, no dead keys)" },
    { "is", "mac_legacy", "mac_legacy                  Icelandic (Macintosh, legacy)" },
    { "is", "mac", "mac                         Icelandic (Macintosh)" },
    { "is", "dvorak", "dvorak                      Icelandic (Dvorak)" },
    { "in", "asm-kagapa", "asm-kagapa                  Assamese (KaGaPa, phonetic)" },
    { "in", "ben", "ben                         Bangla (India)" },
    { "in", "ben_probhat", "ben_probhat                 Bangla (India, Probhat)" },
    { "in", "ben_baishakhi", "ben_baishakhi               Bangla (India, Baishakhi)" },
    { "in", "ben_bornona", "ben_bornona                 Bangla (India, Bornona)" },
    { "in", "ben-kagapa", "ben-kagapa                  Bangla (India, KaGaPa, phonetic)" },
    { "in", "ben_gitanjali", "ben_gitanjali               Bangla (India, Gitanjali)" },
    { "in", "ben_inscript", "ben_inscript                Bangla (India, Baishakhi InScript)" },
    { "in", "eng", "eng                         English (India, with rupee)" },
    { "in", "guj", "guj                         Gujarati" },
    { "in", "guj-kagapa", "guj-kagapa                  Gujarati (KaGaPa, phonetic)" },
    { "in", "bolnagri", "bolnagri                    Hindi (Bolnagri)" },
    { "in", "hin-wx", "hin-wx                      Hindi (Wx)" },
    { "in", "hin-kagapa", "hin-kagapa                  Hindi (KaGaPa, phonetic)" },
    { "in", "kan", "kan                         Kannada" },
    { "in", "kan-kagapa", "kan-kagapa                  Kannada (KaGaPa, phonetic)" },
    { "in", "mal", "mal                         Malayalam" },
    { "in", "mal_lalitha", "mal_lalitha                 Malayalam (Lalitha)" },
    { "in", "mal_enhanced", "mal_enhanced                Malayalam (enhanced InScript, with rupee)" },
    { "in", "mal_poorna", "mal_poorna                  Malayalam (Poorna, extended InScript)" },
    { "in", "mni", "mni                         Manipuri (Meitei)" },
    { "in", "mar-kagapa", "mar-kagapa                  Marathi (KaGaPa, phonetic)" },
    { "in", "marathi", "marathi                     Marathi (enhanced InScript)" },
    { "in", "ori", "ori                         Oriya" },
    { "in", "ori-bolnagri", "ori-bolnagri                Oriya (Bolnagri)" },
    { "in", "ori-wx", "ori-wx                      Oriya (Wx)" },
    { "in", "guru", "guru                        Punjabi (Gurmukhi)" },
    { "in", "jhelum", "jhelum                      Punjabi (Gurmukhi Jhelum)" },
    { "in", "san-kagapa", "san-kagapa                  Sanskrit (KaGaPa, phonetic)" },
    { "in", "sat", "sat                         Santali (Ol Chiki)" },
    { "in", "tamilnet", "tamilnet                    Tamil (TamilNet '99)" },
    { "in", "tamilnet_tamilnumbers", "tamilnet_tamilnumbers       Tamil (TamilNet '99 with Tamil numerals)" },
    { "in", "tamilnet_TAB", "tamilnet_TAB                Tamil (TamilNet '99, TAB encoding)" },
    { "in", "tamilnet_TSCII", "tamilnet_TSCII              Tamil (TamilNet '99, TSCII encoding)" },
    { "in", "tam", "tam                         Tamil (InScript, with Arabic numerals)" },
    { "in", "tam_tamilnumbers", "tam_tamilnumbers            Tamil (InScript, with Tamil numerals)" },
    { "in", "tel", "tel                         Telugu" },
    { "in", "tel-kagapa", "tel-kagapa                  Telugu (KaGaPa, phonetic)" },
    { "in", "tel-sarala", "tel-sarala                  Telugu (Sarala)" },
    { "in", "urd-phonetic", "urd-phonetic                Urdu (phonetic)" },
    { "in", "urd-phonetic3", "urd-phonetic3               Urdu (alt. phonetic)" },
    { "in", "urd-winkeys", "urd-winkeys                 Urdu (Windows)" },
    { "in", "iipa", "iipa                        Indic IPA" },
    { "id", "melayu-phonetic", "melayu-phonetic             Indonesian (Arab Melayu, phonetic)" },
    { "id", "melayu-phoneticx", "melayu-phoneticx            Indonesian (Arab Melayu, extended phonetic)" },
    { "id", "pegon-phonetic", "pegon-phonetic              Indonesian (Arab Pegon, phonetic)" },
    { "id", "javanese", "javanese                    Javanese" },
    { "ie", "UnicodeExpert", "UnicodeExpert               Irish (UnicodeExpert)" },
    { "ie", "CloGaelach", "CloGaelach                  CloGaelach" },
    { "ie", "ogam", "ogam                        Ogham" },
    { "ie", "ogam_is434", "ogam_is434                  Ogham (IS434)" },
    { "it", "nodeadkeys", "nodeadkeys                  Italian (no dead keys)" },
    { "it", "winkeys", "winkeys                     Italian (Windows)" },
    { "it", "mac", "mac                         Italian (Macintosh)" },
    { "it", "us", "us                          Italian (US)" },
    { "it", "ibm", "ibm                         Italian (IBM 142)" },
    { "it", "fur", "fur                         Friulian (Italy)" },
    { "it", "scn", "scn                         Sicilian" },
    { "it", "geo", "geo                         Georgian (Italy)" },
    { "jp", "kana", "kana                        Japanese (Kana)" },
    { "jp", "OADG109A", "OADG109A                    Japanese (OADG 109A)" },
    { "jp", "mac", "mac                         Japanese (Macintosh)" },
    { "jp", "dvorak", "dvorak                      Japanese (Dvorak)" },
    { "kz", "kazrus", "kazrus                      Kazakh (with Russian)" },
    { "kz", "ext", "ext                         Kazakh (extended)" },
    { "kz", "latin", "latin                       Kazakh (Latin)" },
    { "kz", "ruskaz", "ruskaz                      Russian (Kazakhstan, with Kazakh)" },
    { "kr", "kr104", "kr104                       Korean (101/104-key compatible)" },
    { "kg", "phonetic", "phonetic                    Kyrgyz (phonetic)" },
    { "la", "stea", "stea                        Lao (STEA)" },
    { "lv", "apostrophe", "apostrophe                  Latvian (apostrophe)" },
    { "lv", "tilde", "tilde                       Latvian (tilde)" },
    { "lv", "fkey", "fkey                        Latvian (F)" },
    { "lv", "modern", "modern                      Latvian (Modern Latin)" },
    { "lv", "modern-cyr", "modern-cyr                  Latvian (Modern Cyrillic)" },
    { "lv", "ergonomic", "ergonomic                   Latvian (ergonomic, UGJRMV)" },
    { "lv", "adapted", "adapted                     Latvian (adapted)" },
    { "lt", "std", "std                         Lithuanian (standard)" },
    { "lt", "us", "us                          Lithuanian (US)" },
    { "lt", "ibm", "ibm                         Lithuanian (IBM)" },
    { "lt", "lekp", "lekp                        Lithuanian (LEKP)" },
    { "lt", "lekpa", "lekpa                       Lithuanian (LEKPa)" },
    { "lt", "ratise", "ratise                      Lithuanian (Ratise)" },
    { "lt", "sgs", "sgs                         Samogitian" },
    { "mk", "nodeadkeys", "nodeadkeys                  Macedonian (no dead keys)" },
    { "my", "phonetic", "phonetic                    Malay (Jawi, phonetic)" },
    { "mt", "us", "us                          Maltese (US)" },
    { "mt", "alt-us", "alt-us                      Maltese (US, with AltGr overrides)" },
    { "mt", "alt-gb", "alt-gb                      Maltese (UK, with AltGr overrides)" },
    { "md", "gag", "gag                         Gagauz (Moldova)" },
    { "me", "cyrillic", "cyrillic                    Montenegrin (Cyrillic)" },
    { "me", "cyrillicyz", "cyrillicyz                  Montenegrin (Cyrillic, ZE and ZHE swapped)" },
    { "me", "cyrillicalternatequotes", "cyrillicalternatequotes     Montenegrin (Cyrillic, with guillemets)" },
    { "me", "latinunicode", "latinunicode                Montenegrin (Latin, Unicode)" },
    { "me", "latinyz", "latinyz                     Montenegrin (Latin, QWERTY)" },
    { "me", "latinunicodeyz", "latinunicodeyz              Montenegrin (Latin, Unicode, QWERTY)" },
    { "me", "latinalternatequotes", "latinalternatequotes        Montenegrin (Latin, with guillemets)" },
    { "no", "nodeadkeys", "nodeadkeys                  Norwegian (no dead keys)" },
    { "no", "winkeys", "winkeys                     Norwegian (Windows)" },
    { "no", "mac", "mac                         Norwegian (Macintosh)" },
    { "no", "mac_nodeadkeys", "mac_nodeadkeys              Norwegian (Macintosh, no dead keys)" },
    { "no", "colemak", "colemak                     Norwegian (Colemak)" },
    { "no", "colemak_dh", "colemak_dh                  Norwegian (Colemak-DH)" },
    { "no", "colemak_dh_wide", "colemak_dh_wide             Norwegian (Colemak-DH Wide)" },
    { "no", "dvorak", "dvorak                      Norwegian (Dvorak)" },
    { "no", "smi", "smi                         Northern Saami (Norway)" },
    { "no", "smi_nodeadkeys", "smi_nodeadkeys              Northern Saami (Norway, no dead keys)" },
    { "ir", "pes_keypad", "pes_keypad                  Persian (with Persian keypad)" },
    { "ir", "winkeys", "winkeys                     Persian (Windows)" },
    { "ir", "azb", "azb                         Azerbaijani (Iran)" },
    { "ir", "ku", "ku                          Kurdish (Iran, Latin Q)" },
    { "ir", "ku_alt", "ku_alt                      Kurdish (Iran, Latin Alt-Q)" },
    { "ir", "ku_f", "ku_f                        Kurdish (Iran, F)" },
    { "ir", "ku_ara", "ku_ara                      Kurdish (Iran, Arabic-Latin)" },
    { "pl", "legacy", "legacy                      Polish (legacy)" },
    { "pl", "qwertz", "qwertz                      Polish (QWERTZ)" },
    { "pl", "dvorak", "dvorak                      Polish (Dvorak)" },
    { "pl", "dvorak_quotes", "dvorak_quotes               Polish (Dvorak, with Polish quotes on quotemark key)" },
    { "pl", "dvorak_altquotes", "dvorak_altquotes            Polish (Dvorak, with Polish quotes on key 1)" },
    { "pl", "dvp", "dvp                         Polish (programmer Dvorak)" },
    { "pl", "csb", "csb                         Kashubian" },
    { "pl", "szl", "szl                         Silesian" },
    { "pl", "ru_phonetic_dvorak", "ru_phonetic_dvorak          Russian (Poland, phonetic Dvorak)" },
    { "pt", "nodeadkeys", "nodeadkeys                  Portuguese (no dead keys)" },
    { "pt", "mac", "mac                         Portuguese (Macintosh)" },
    { "pt", "mac_nodeadkeys", "mac_nodeadkeys              Portuguese (Macintosh, no dead keys)" },
    { "pt", "nativo", "nativo                      Portuguese (Nativo)" },
    { "pt", "nativo-us", "nativo-us                   Portuguese (Nativo for US keyboards)" },
    { "pt", "nativo-epo", "nativo-epo                  Esperanto (Portugal, Nativo)" },
    { "br", "nodeadkeys", "nodeadkeys                  Portuguese (Brazil, no dead keys)" },
    { "br", "dvorak", "dvorak                      Portuguese (Brazil, Dvorak)" },
    { "br", "nativo", "nativo                      Portuguese (Brazil, Nativo)" },
    { "br", "nativo-us", "nativo-us                   Portuguese (Brazil, Nativo for US keyboards)" },
    { "br", "thinkpad", "thinkpad                    Portuguese (Brazil, IBM/Lenovo ThinkPad)" },
    { "br", "nativo-epo", "nativo-epo                  Esperanto (Brazil, Nativo)" },
    { "br", "rus", "rus                         Russian (Brazil, phonetic)" },
    { "ro", "std", "std                         Romanian (standard)" },
    { "ro", "winkeys", "winkeys                     Romanian (Windows)" },
    { "ru", "phonetic", "phonetic                    Russian (phonetic)" },
    { "ru", "phonetic_winkeys", "phonetic_winkeys            Russian (phonetic, Windows)" },
    { "ru", "phonetic_YAZHERTY", "phonetic_YAZHERTY           Russian (phonetic, YAZHERTY)" },
    { "ru", "phonetic_azerty", "phonetic_azerty             Russian (phonetic, AZERTY)" },
    { "ru", "phonetic_dvorak", "phonetic_dvorak             Russian (phonetic, Dvorak)" },
    { "ru", "typewriter", "typewriter                  Russian (typewriter)" },
    { "ru", "ruchey_ru", "ruchey_ru                   Russian (engineering, RU)" },
    { "ru", "ruchey_en", "ruchey_en                   Russian (engineering, EN)" },
    { "ru", "legacy", "legacy                      Russian (legacy)" },
    { "ru", "typewriter-legacy", "typewriter-legacy           Russian (typewriter, legacy)" },
    { "ru", "dos", "dos                         Russian (DOS)" },
    { "ru", "mac", "mac                         Russian (Macintosh)" },
    { "ru", "ab", "ab                          Abkhazian (Russia)" },
    { "ru", "bak", "bak                         Bashkirian" },
    { "ru", "cv", "cv                          Chuvash" },
    { "ru", "cv_latin", "cv_latin                    Chuvash (Latin)" },
    { "ru", "xal", "xal                         Kalmyk" },
    { "ru", "kom", "kom                         Komi" },
    { "ru", "chm", "chm                         Mari" },
    { "ru", "os_legacy", "os_legacy                   Ossetian (legacy)" },
    { "ru", "os_winkeys", "os_winkeys                  Ossetian (Windows)" },
    { "ru", "srp", "srp                         Serbian (Russia)" },
    { "ru", "tt", "tt                          Tatar" },
    { "ru", "udm", "udm                         Udmurt" },
    { "ru", "sah", "sah                         Yakut" },
    { "rs", "alternatequotes", "alternatequotes             Serbian (Cyrillic, with guillemets)" },
    { "rs", "yz", "yz                          Serbian (Cyrillic, ZE and ZHE swapped)" },
    { "rs", "latin", "latin                       Serbian (Latin)" },
    { "rs", "latinalternatequotes", "latinalternatequotes        Serbian (Latin, with guillemets)" },
    { "rs", "latinunicode", "latinunicode                Serbian (Latin, Unicode)" },
    { "rs", "latinyz", "latinyz                     Serbian (Latin, QWERTY)" },
    { "rs", "latinunicodeyz", "latinunicodeyz              Serbian (Latin, Unicode, QWERTY)" },
    { "rs", "rue", "rue                         Pannonian Rusyn" },
    { "lk", "us", "us                          Sinhala (US)" },
    { "lk", "tam_unicode", "tam_unicode                 Tamil (Sri Lanka, TamilNet '99)" },
    { "lk", "tam_TAB", "tam_TAB                     Tamil (Sri Lanka, TamilNet '99, TAB encoding)" },
    { "sk", "bksl", "bksl                        Slovak (extra backslash)" },
    { "sk", "qwerty", "qwerty                      Slovak (QWERTY)" },
    { "sk", "qwerty_bksl", "qwerty_bksl                 Slovak (QWERTY, extra backslash)" },
    { "si", "alternatequotes", "alternatequotes             Slovenian (with guillemets)" },
    { "si", "us", "us                          Slovenian (US)" },
    { "es", "nodeadkeys", "nodeadkeys                  Spanish (no dead keys)" },
    { "es", "deadtilde", "deadtilde                   Spanish (dead tilde)" },
    { "es", "winkeys", "winkeys                     Spanish (Windows)" },
    { "es", "dvorak", "dvorak                      Spanish (Dvorak)" },
    { "es", "ast", "ast                         Asturian (Spain, with bottom-dot H and L)" },
    { "es", "cat", "cat                         Catalan (Spain, with middle-dot L)" },
    { "latam", "nodeadkeys", "nodeadkeys                  Spanish (Latin American, no dead keys)" },
    { "latam", "deadtilde", "deadtilde                   Spanish (Latin American, dead tilde)" },
    { "latam", "dvorak", "dvorak                      Spanish (Latin American, Dvorak)" },
    { "latam", "colemak", "colemak                     Spanish (Latin American, Colemak)" },
    { "ke", "kik", "kik                         Kikuyu" },
    { "se", "nodeadkeys", "nodeadkeys                  Swedish (no dead keys)" },
    { "se", "dvorak", "dvorak                      Swedish (Dvorak)" },
    { "se", "us_dvorak", "us_dvorak                   Swedish (Dvorak, intl.)" },
    { "se", "svdvorak", "svdvorak                    Swedish (Svdvorak)" },
    { "se", "colemak", "colemak                     Swedish (Colemak)" },
    { "se", "mac", "mac                         Swedish (Macintosh)" },
    { "se", "us", "us                          Swedish (US)" },
    { "se", "swl", "swl                         Swedish Sign Language" },
    { "se", "smi", "smi                         Northern Saami (Sweden)" },
    { "se", "rus", "rus                         Russian (Sweden, phonetic)" },
    { "tw", "indigenous", "indigenous                  Taiwanese (indigenous)" },
    { "tw", "saisiyat", "saisiyat                    Saisiyat (Taiwan)" },
    { "tj", "legacy", "legacy                      Tajik (legacy)" },
    { "th", "tis", "tis                         Thai (TIS-820.2538)" },
    { "th", "pat", "pat                         Thai (Pattachote)" },
    { "tm", "alt", "alt                         Turkmen (Alt-Q)" },
    { "tr", "f", "f                           Turkish (F)" },
    { "tr", "e", "e                           Turkish (E)" },
    { "tr", "alt", "alt                         Turkish (Alt-Q)" },
    { "tr", "intl", "intl                        Turkish (intl., with dead keys)" },
    { "tr", "ku", "ku                          Kurdish (Turkey, Latin Q)" },
    { "tr", "ku_f", "ku_f                        Kurdish (Turkey, F)" },
    { "tr", "ku_alt", "ku_alt                      Kurdish (Turkey, Latin Alt-Q)" },
    { "ua", "phonetic", "phonetic                    Ukrainian (phonetic)" },
    { "ua", "typewriter", "typewriter                  Ukrainian (typewriter)" },
    { "ua", "winkeys", "winkeys                     Ukrainian (Windows)" },
    { "ua", "macOS", "macOS                       Ukrainian (macOS)" },
    { "ua", "legacy", "legacy                      Ukrainian (legacy)" },
    { "ua", "homophonic", "homophonic                  Ukrainian (homophonic)" },
    { "ua", "crh", "crh                         Crimean Tatar (Turkish Q)" },
    { "ua", "crh_f", "crh_f                       Crimean Tatar (Turkish F)" },
    { "ua", "crh_alt", "crh_alt                     Crimean Tatar (Turkish Alt-Q)" },
    { "pk", "urd-crulp", "urd-crulp                   Urdu (Pakistan, CRULP)" },
    { "pk", "urd-nla", "urd-nla                     Urdu (Pakistan, NLA)" },
    { "pk", "ara", "ara                         Arabic (Pakistan)" },
    { "pk", "snd", "snd                         Sindhi" },
    { "uz", "latin", "latin                       Uzbek (Latin)" },
    { "vn", "us", "us                          Vietnamese (US)" },
    { "vn", "fr", "fr                          Vietnamese (France)" },
};
#define VARIANT_COUNT (sizeof(variants) / sizeof(variants[0]))

/* ------------------------------------------------------------------ */
/* Structured -xkboptions data.  One group per <group> in              */
/* rules/xorg.xml <optionList>; the codes and descriptions are copied  */
/* verbatim from that file so the UI can only ever emit a value the    */
/* server accepts.  `multiple` mirrors allowMultipleSelection.         */
/* ------------------------------------------------------------------ */

struct xkb_option {
    const char *code;   /* e.g. "grp:switch" */
    const char *name;   /* display description */
    const char *tooltip;   /* display tooltip */
};

struct xkb_option_group {
    const char *title;
    const struct xkb_option *opts;
    int count;
    int multiple;       /* 1 = checkboxes, 0 = single-choice dropdown */
};

static const struct xkb_option grp_opts[] = {
    { "grp:switch", "Right Alt (while pressed)" },
    { "grp:lswitch", "Left Alt (while pressed)" },
    { "grp:lwin_switch", "Left Win (while pressed)" },
    { "grp:rwin_switch", "Right Win (while pressed)" },
    { "grp:win_switch", "Any Win (while pressed)" },
    { "grp:menu_switch", "Menu (while pressed), Shift+Menu for Menu" },
    { "grp:caps_switch", "Caps Lock (while pressed), Alt+Caps Lock for ...", "Caps Lock (while pressed), Alt+Caps Lock\nfor the original Caps Lock action" },
    { "grp:rctrl_switch", "Right Ctrl (while pressed)" },
    { "grp:toggle", "Right Alt" },
    { "grp:lalt_toggle", "Left Alt" },
    { "grp:caps_toggle", "Caps Lock" },
    { "grp:shift_caps_toggle", "Shift+Caps Lock" },
    { "grp:caps_select", "Caps Lock to first layout; Shift+Caps Lock to second layout", "Caps Lock to first layout;\nShift+Caps Lock to second layout" },
    { "grp:win_menu_select", "Left Win to first layout; Right Win/Menu to second layout", "Left Win to first layout;\nRight Win/Menu to second layout" },
    { "grp:ctrl_select", "Left Ctrl to first layout; Right Ctrl to second layout", "Left Ctrl to first layout;\nRight Ctrl to second layout" },
    { "grp:alt_caps_toggle", "Alt+Caps Lock" },
    { "grp:shifts_toggle", "Both Shifts together" },
    { "grp:alts_toggle", "Both Alts together" },
    { "grp:alt_altgr_toggle", "Both Alts together; AltGr alone chooses third level" },
    { "grp:ctrls_toggle", "Both Ctrls together" },
    { "grp:ctrl_shift_toggle", "Ctrl+Shift" },
    { "grp:lctrl_lshift_toggle", "Left Ctrl+Left Shift" },
    { "grp:rctrl_rshift_toggle", "Right Ctrl+Right Shift" },
    { "grp:ctrl_shift_toggle_bidir", "Left Ctrl+Left Shift chooses previous layout, Right ...", "Left Ctrl+Left Shift chooses previous layout,\nRight Ctrl+Right Shift chooses next layout" },
    { "grp:ctrl_alt_toggle", "Alt+Ctrl" },
    { "grp:lctrl_lalt_toggle", "Left Alt+Left Ctrl" },
    { "grp:rctrl_ralt_toggle", "Right Alt+Right Ctrl" },
    { "grp:ctrl_alt_toggle_bidir", "Left Ctrl+Left Alt chooses previous layout, Right ...", "Left Ctrl+Left Alt chooses previous layout,\nRight Ctrl+Right Alt chooses next layout" },
    { "grp:alt_shift_toggle", "Alt+Shift" },
    { "grp:lalt_lshift_toggle", "Left Alt+Left Shift" },
    { "grp:ralt_rshift_toggle", "Right Alt+Right Shift" },
    { "grp:alt_shift_toggle_bidir", "Left Alt+Left Shift chooses previous layout, Right ...", "Left Alt+Left Shift chooses previous layout,\n Right Alt+Right Shift chooses next layout" },
    { "grp:menu_toggle", "Menu" },
    { "grp:lwin_toggle", "Left Win" },
    { "grp:alt_space_toggle", "Alt+Space" },
    { "grp:win_space_toggle", "Win+Space" },
    { "grp:ctrl_space_toggle", "Ctrl+Space" },
    { "grp:rwin_toggle", "Right Win" },
    { "grp:lshift_toggle", "Left Shift" },
    { "grp:rshift_toggle", "Right Shift" },
    { "grp:lctrl_toggle", "Left Ctrl" },
    { "grp:rctrl_toggle", "Right Ctrl" },
    { "grp:sclk_toggle", "Scroll Lock" },
    { "grp:lctrl_lwin_rctrl_menu", "Ctrl+Left Win to first layout; Ctrl+Menu to second layout", "Ctrl+Left Win to first layout;\nCtrl+Menu to second layout" },
    { "grp:lctrl_lwin_toggle", "Left Ctrl+Left Win" },
};

static const struct xkb_option lv2_opts[] = {
    { "lv2:lsgt_switch", "The \"< >\" key" },
};

static const struct xkb_option lv3_opts[] = {
    { "lv3:switch", "Right Ctrl" },
    { "lv3:menu_switch", "Menu" },
    { "lv3:win_switch", "Any Win" },
    { "lv3:lwin_switch", "Left Win" },
    { "lv3:rwin_switch", "Right Win" },
    { "lv3:alt_switch", "Any Alt" },
    { "lv3:lalt_switch", "Left Alt" },
    { "lv3:ralt_switch", "Right Alt" },
    { "lv3:ralt_switch_multikey", "Right Alt; Shift+Right Alt as Compose" },
    { "lv3:ralt_alt", "Right Alt never chooses 3rd level" },
    { "lv3:enter_switch", "Enter on keypad" },
    { "lv3:caps_switch", "Caps Lock" },
    { "lv3:caps_switch_capslock_with_ctrl", "Caps Lock; Ctrl+Caps Lock for original Caps Lock action", "Caps Lock; Ctrl+Caps Lock for\noriginal Caps Lock action" },
    { "lv3:bksl_switch", "Backslash" },
    { "lv3:lsgt_switch", "The \"< >\" key" },
    { "lv3:caps_switch_latch", "Caps Lock; acts as onetime lock when pressed ...", "Caps Lock; acts as onetime lock when pressed\ntogether with another 3rd-level chooser" },
    { "lv3:bksl_switch_latch", "Backslash; acts as onetime lock when pressed ...", "Backslash; acts as onetime lock when pressed\ntogether with another 3rd level chooser" },
    { "lv3:lsgt_switch_latch", "The \"< >\" key; acts as onetime lock when ...", "The \"< >\" key; acts as onetime lock when pressed\ntogether with another 3rd level chooser" },
};

static const struct xkb_option ctrl_opts[] = {
    { "ctrl:nocaps", "Caps Lock as Ctrl" },
    { "ctrl:lctrl_meta", "Left Ctrl as Meta" },
    { "ctrl:swapcaps", "Swap Ctrl and Caps Lock" },
    { "ctrl:grouptoggle_capscontrol", "Caps Lock as Ctrl, Left Control switches to another layout", "Caps Lock as Ctrl, Left Control\nswitches to another layout" },
    { "ctrl:hyper_capscontrol", "Caps Lock as Ctrl, Ctrl as Hyper" },
    { "ctrl:ac_ctrl", "To the left of \"A\"" },
    { "ctrl:aa_ctrl", "At the bottom left" },
    { "ctrl:rctrl_ralt", "Right Ctrl as Right Alt" },
    { "ctrl:ralt_rctrl", "Right Alt as Right Control" },
    { "ctrl:menu_rctrl", "Menu as Right Ctrl" },
    { "ctrl:swap_lalt_lctl", "Swap Left Alt with Left Ctrl" },
    { "ctrl:swap_ralt_rctl", "Swap Right Alt with Right Ctrl" },
    { "ctrl:swap_lwin_lctl", "Swap Left Win with Left Ctrl" },
    { "ctrl:swap_rwin_rctl", "Swap Right Win with Right Ctrl" },
    { "ctrl:swap_lalt_lctl_lwin", "Left Alt as Ctrl, Left Ctrl as Win, Left Win as Left Alt", "Left Alt as Ctrl, Left Ctrl as Win,\nLeft Win as Left Alt" },
};

static const struct xkb_option grp_led_opts[] = {
    { "grp_led:num", "Num Lock" },
    { "grp_led:caps", "Caps Lock" },
    { "grp_led:scroll", "Scroll Lock" },
};

static const struct xkb_option mod_led_opts[] = {
    { "mod_led:compose", "Compose" },
};

static const struct xkb_option keypad_opts[] = {
    { "keypad:legacy", "Legacy" },
    { "keypad:oss", "Unicode arrows and math operators" },
    { "keypad:future", "Unicode arrows and math operators on default level" },
    { "keypad:legacy_wang", "Legacy Wang 724" },
    { "keypad:oss_wang", "Wang 724 keypad with Unicode arrows and math operators" },
    { "keypad:future_wang", "Wang 724 keypad with Unicode arrows and math operators on default level" },
    { "keypad:hex", "Hexadecimal" },
    { "keypad:atm", "Phone and ATM style" },
};

static const struct xkb_option kpdl_opts[] = {
    { "kpdl:dot", "Legacy key with dot" },
    { "kpdl:comma", "Legacy key with comma" },
    { "kpdl:dotoss", "Four-level key with dot" },
    { "kpdl:dotoss_latin9", "Four-level key with dot, Latin-9 only" },
    { "kpdl:commaoss", "Four-level key with comma" },
    { "kpdl:momayyezoss", "Four-level key with momayyez" },
    { "kpdl:kposs", "Four-level key with abstract separators" },
    { "kpdl:semi", "Semicolon on third level" },
};

static const struct xkb_option caps_opts[] = {
    { "caps:internal", "Caps Lock uses internal capitalization; Shift \"pauses\" Caps Lock" },
    { "caps:internal_nocancel", "Caps Lock uses internal capitalization; Shift does ...", "Caps Lock uses internal capitalization;\nShift does not affect Caps Lock" },
    { "caps:shift", "Caps Lock acts as Shift with locking; Shift \"pauses\" Caps Lock", "Caps Lock acts as Shift with locking;\nShift \"pauses\" Caps Lock" },
    { "caps:shift_nocancel", "Caps Lock acts as Shift with locking; Shift does not ...", "Caps Lock acts as Shift with locking;\n Shift does not affect Caps Lock" },
    { "caps:capslock", "Caps Lock toggles normal capitalization of alphabetic ...", "Caps Lock toggles normal capitalization of\nalphabetic characters" },
    { "caps:shiftlock", "Caps Lock toggles Shift Lock (affects all keys)", "Caps Lock toggles Shift Lock\n(affects all keys)" },
    { "caps:swapescape", "Swap Esc and Caps Lock" },
    { "caps:escape", "Make Caps Lock an additional Esc" },
    { "caps:escape_shifted_capslock", "Make Caps Lock an additional Esc, but Shift+Caps ...", "Make Caps Lock an additional Esc, but\nShift+Caps Lock is the regular Caps Lock" },
    { "caps:backspace", "Make Caps Lock an additional Backspace" },
    { "caps:return", "Make Caps Lock an additional Return key" },
    { "caps:super", "Make Caps Lock an additional Super" },
    { "caps:hyper", "Make Caps Lock an additional Hyper" },
    { "caps:menu", "Make Caps Lock an additional Menu key" },
    { "caps:numlock", "Make Caps Lock an additional Num Lock" },
    { "caps:ctrl_shifted_capslock", "Make Caps Lock an additional Ctrl and Shift+Caps Lock ...", "Make Caps Lock an additional Ctrl and\nShift+Caps Lock the regular Caps Lock" },
    { "caps:ctrl_modifier", "Make Caps Lock act as an additional Ctrl modifier, but ...", "Make Caps Lock act as an additional Ctrl modifier,\nbut keep identifying as Caps Lock" },
    { "caps:digits_row", "Caps Lock gives digits on the digits row (Azerty layouts)" },
    { "caps:digits_row_independent_lock", "Shift+Caps locks the digits on the digits row, Caps ...", "Shift+Caps locks the digits on the digits row, Caps Lock\nalone behaves as usual (Azerty layouts)" },
    { "caps:none", "Caps Lock is disabled" },
};

static const struct xkb_option altwin_opts[] = {
    { "altwin:menu", "Add the standard behavior to Menu key" },
    { "altwin:menu_win", "Menu is mapped to Win" },
    { "altwin:meta_alt", "Alt and Meta are on Alt" },
    { "altwin:alt_win", "Alt is mapped to Win and the usual Alt" },
    { "altwin:ctrl_win", "Ctrl is mapped to Win and the usual Ctrl" },
    { "altwin:ctrl_rwin", "Ctrl is mapped to Right Win and the usual Ctrl" },
    { "altwin:ctrl_alt_win", "Ctrl is mapped to Alt, Alt to Win" },
    { "altwin:meta_win", "Meta is mapped to Win" },
    { "altwin:left_meta_win", "Meta is mapped to Left Win" },
    { "altwin:hyper_win", "Hyper is mapped to Win" },
    { "altwin:alt_super_win", "Alt is mapped to Right Win, Super to Menu" },
    { "altwin:swap_lalt_lwin", "Left Alt is swapped with Left Win" },
    { "altwin:swap_ralt_rwin", "Right Alt is swapped with Right Win" },
    { "altwin:swap_alt_win", "Alt is swapped with Win" },
    { "altwin:prtsc_rwin", "Win is mapped to PrtSc and the usual Win" },
};

static const struct xkb_option compose_opts[] = {
    { "compose:ralt", "Right Alt" },
    { "compose:lwin", "Left Win" },
    { "compose:lwin-altgr", "3rd level of Left Win" },
    { "compose:rwin", "Right Win" },
    { "compose:rwin-altgr", "3rd level of Right Win" },
    { "compose:menu", "Menu" },
    { "compose:menu-altgr", "3rd level of Menu" },
    { "compose:lctrl", "Left Ctrl" },
    { "compose:lctrl-altgr", "3rd level of Left Ctrl" },
    { "compose:rctrl", "Right Ctrl" },
    { "compose:rctrl-altgr", "3rd level of Right Ctrl" },
    { "compose:caps", "Caps Lock" },
    { "compose:caps-altgr", "3rd level of Caps Lock" },
    { "compose:102", "The \"< >\" key" },
    { "compose:102-altgr", "3rd level of the \"< >\" key" },
    { "compose:paus", "Pause" },
    { "compose:ins", "Insert" },
    { "compose:prsc", "PrtSc" },
    { "compose:sclk", "Scroll Lock" },
};

static const struct xkb_option compat_opts[] = {
    { "numpad:pc", "Default numeric keypad keys" },
    { "numpad:mac", "Numeric keypad always enters digits (as in macOS)" },
    { "numpad:microsoft", "Num Lock on: digits; Shift for arrows. Num Lock...", "Num Lock on: digits; Shift for arrows.\nNum Lock off: arrows (as in Windows)" },
    { "numpad:shift3", "Shift does not cancel Num Lock, chooses 3rd level instead", "Shift does not cancel Num Lock,\nchooses 3rd level instead" },
    { "hyper:mod3", "Map Hyper to Mod3 (conflict with LevelFive)" },
    { "hyper:mod4", "Map Hyper to Mod4 (conflict with Super)" },
    { "scrolllock:mod3", "Map Scroll Lock to Mod3" },
    { "srvrkeys:none", "Special keys (Ctrl+Alt+<key>) handled in a server" },
    { "apple:alupckeys", "Apple Aluminium emulates Pause, PrtSc, Scroll Lock" },
    { "apple:jp_oadg109a", "Japanese Apple keyboards emulate OADG109A backslash", "Japanese Apple keyboards emulate\nOADG109A backslash" },
    { "apple:jp_pc106", "Japanese Apple keyboards emulate PC106 backslash" },
    { "shift:breaks_caps", "Shift cancels Caps Lock" },
    { "misc:typo", "Enable extra typographic characters" },
    { "misc:apl", "Enable APL overlay characters" },
    { "shift:both_capslock", "Both Shifts together enable Caps Lock" },
    { "shift:both_capslock_cancel", "Both Shifts together enable Caps Lock...", "Both Shifts together enable Caps Lock;\none Shift key disables it" },
    { "shift:both_shiftlock", "Both Shifts together enable Shift Lock" },
    { "keypad:pointerkeys", "Shift+Num Lock enables PointerKeys" },
    { "grab:break_actions", "Allow breaking grabs with keyboard actions", "Allow breaking grabs with keyboard\nactions (warning: security risk)" },
    { "grab:debug", "Allow grab and window tree logging" },
};

static const struct xkb_option currencysign_opts[] = {
    { "eurosign:e", "Euro on E, third level" },
    { "eurosign:E", "Euro on E, fourth level" },
    { "eurosign:2", "Euro on 2" },
    { "eurosign:4", "Euro on 4" },
    { "eurosign:5", "Euro on 5" },
    { "rupeesign:4", "Rupee on 4" },
};

static const struct xkb_option lv5_opts[] = {
    { "lv5:caps_switch", "Caps Lock chooses 5th level" },
    { "lv5:lsgt_switch", "The \"< >\" key chooses 5th level" },
    { "lv5:ralt_switch", "Right Alt chooses 5th level" },
    { "lv5:menu_switch", "Menu chooses 5th level" },
    { "lv5:rctrl_switch", "Right Ctrl chooses 5th level" },
    { "lv5:lsgt_switch_lock", "The \"< >\" key chooses 5th level and ...", "The \"< >\" key chooses 5th level and acts as a one-time\nlock if pressed with another 5th level chooser" },
    { "lv5:ralt_switch_lock", "Right Alt chooses 5th level and acts ...", "Right Alt chooses 5th level and acts as a one-time lock\nif pressed with another 5th level chooser" },
    { "lv5:lwin_switch_lock", "Left Win chooses 5th level and acts ...", "Left Win chooses 5th level and acts as a one-time lock\nif pressed with another 5th level chooser" },
    { "lv5:rwin_switch_lock", "Right Win chooses 5th level and acts ...", "Right Win chooses 5th level and acts as a one-time lock\nif pressed with another 5th level chooser" },
};

static const struct xkb_option nbsp_opts[] = {
    { "nbsp:none", "Usual space at any level" },
    { "nbsp:level2", "Non-breaking space at the 2nd level" },
    { "nbsp:level3", "Non-breaking space at the 3rd level" },
    { "nbsp:level3n", "Non-breaking space at the 3rd level, thin non-breaking space at the 4th level" },
    { "nbsp:level4", "Non-breaking space at the 4th level" },
    { "nbsp:level4n", "Non-breaking space at the 4th level, thin non-breaking space at the 6th level", "Non-breaking space at the 4th level,\nthin non-breaking space at the 6th level" },
    { "nbsp:level4nl", "Non-breaking space at the 4th level, thin non-breaking space at the 6th level (via Ctrl+Shift)", "Non-breaking space at the 4th level,\nthin non-breaking space at the 6th level (via Ctrl+Shift)" },
    { "nbsp:zwnj2", "Zero-width non-joiner at the 2nd level" },
    { "nbsp:zwnj2zwj3", "Zero-width non-joiner at the 2nd level, zero-width joiner at the 3rd level", "Zero-width non-joiner at the 2nd level,\nzero-width joiner at the 3rd level" },
    { "nbsp:zwnj2zwj3nb4", "Zero-width non-joiner at the 2nd level, zero-width joiner at the 3rd level, non-breaking space at the 4th level", "Zero-width non-joiner at the 2nd level,\nzero-width joiner at the 3rd level,\nnon-breaking space at the 4th level" },
    { "nbsp:zwnj2nb3", "Zero-width non-joiner at the 2nd level, non-breaking space at the 3rd level", "Zero-width non-joiner at the 2nd level,\nnon-breaking space at the 3rd level" },
    { "nbsp:zwnj2nb3zwj4", "Zero-width non-joiner at the 2nd level, non-breaking space at the 3rd level, zero-width joiner at the 4th level", "Zero-width non-joiner at the 2nd level,\nnon-breaking space at the 3rd level,\nzero-width joiner at the 4th level" },
    { "nbsp:zwnj2nb3nnb4", "Zero-width non-joiner at the 2nd level, non-breaking space at the 3rd level, thin non-breaking space at the 4th level", "Zero-width non-joiner at the 2nd level,\nnon-breaking space at the 3rd level,\nthin non-breaking space at the 4th level" },
    { "nbsp:zwnj3zwj4", "Zero-width non-joiner at the 3rd level, zero-width joiner at the 4th level", "Zero-width non-joiner at the 3rd level,\nzero-width joiner at the 4th level" },
};

static const struct xkb_option japan_opts[] = {
    { "japan:kana_lock", "Kana Lock key is locking" },
    { "japan:nicola_f_bs", "NICOLA-F style Backspace" },
    { "japan:hztg_escape", "Make Zenkaku Hankaku an additional Esc" },
};

static const struct xkb_option korean_opts[] = {
    { "korean:ralt_hangul", "Make right Alt a Hangul key" },
    { "korean:rctrl_hangul", "Make right Ctrl a Hangul key" },
    { "korean:ralt_hanja", "Make right Alt a Hanja key" },
    { "korean:rctrl_hanja", "Make right Ctrl a Hanja key" },
};

static const struct xkb_option esperanto_opts[] = {
    { "esperanto:qwerty", "At the corresponding key in a QWERTY layout" },
    { "esperanto:dvorak", "At the corresponding key in a Dvorak layout" },
    { "esperanto:colemak", "At the corresponding key in a Colemak layout" },
};

static const struct xkb_option solaris_opts[] = {
    { "solaris:sun_compat", "Sun key compatibility" },
};

static const struct xkb_option terminate_opts[] = {
    { "terminate:ctrl_alt_bksp", "Ctrl+Alt+Backspace" },
};

static const struct xkb_option fkeys_opts[] = {
    { "fkeys:basic_13-24", "Use F13-F24 as usual function keys" },
};

static const struct xkb_option custom_opts[] = {
    { "custom:types", "Use user-defined custom XKB types", "\
Under -xkbdir add a file /types/custom which must be\n\
a valid xkb_types component file. You can also set 'custom'\n\
in the [Layout] drop-down and add a file /symbols/custom\n\
which must be a valid XKB symbols component file" },
};

#define XKB_ARR_N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const struct xkb_option_group xkb_groups[] = {
    { "Switching to another layout", grp_opts, XKB_ARR_N(grp_opts), 1 },
    { "Key to choose the 2nd level", lv2_opts, XKB_ARR_N(lv2_opts), 1 },
    { "Key to choose the 3rd level", lv3_opts, XKB_ARR_N(lv3_opts), 1 },
    { "Ctrl position", ctrl_opts, XKB_ARR_N(ctrl_opts), 1 },
    { "Use keyboard LED to show alternative layout", grp_led_opts, XKB_ARR_N(grp_led_opts), 1 },
    { "Use keyboard LED to indicate modifiers", mod_led_opts, XKB_ARR_N(mod_led_opts), 1 },
    { "Layout of numeric keypad", keypad_opts, XKB_ARR_N(keypad_opts), 0 },
    { "Numeric keypad Delete behavior", kpdl_opts, XKB_ARR_N(kpdl_opts), 0 },
    { "Caps Lock behavior", caps_opts, XKB_ARR_N(caps_opts), 0 },
    { "Alt and Win behavior", altwin_opts, XKB_ARR_N(altwin_opts), 0 },
    { "Position of Compose key", compose_opts, XKB_ARR_N(compose_opts), 1 },
    { "Compatibility options", compat_opts, XKB_ARR_N(compat_opts), 1 },
    { "Currency signs", currencysign_opts, XKB_ARR_N(currencysign_opts), 1 },
    { "Key to choose the 5th level", lv5_opts, XKB_ARR_N(lv5_opts), 1 },
    { "Non-breaking space input", nbsp_opts, XKB_ARR_N(nbsp_opts), 0 },
    { "Japanese keyboard options", japan_opts, XKB_ARR_N(japan_opts), 1 },
    { "Korean Hangul/Hanja keys", korean_opts, XKB_ARR_N(korean_opts), 1 },
    { "Esperanto letters with superscripts", esperanto_opts, XKB_ARR_N(esperanto_opts), 0 },
    { "Old Solaris keycodes compatibility", solaris_opts, XKB_ARR_N(solaris_opts), 1 },
    { "Key sequence to kill the X server", terminate_opts, XKB_ARR_N(terminate_opts), 1 },
    { "Function keys", fkeys_opts, XKB_ARR_N(fkeys_opts), 1 },
    { "Miscellaneous options", custom_opts, XKB_ARR_N(custom_opts), 1 },
};

#define XKB_GROUP_COUNT ((int)(sizeof(xkb_groups) / sizeof(xkb_groups[0])))

/* Compile-time guard: the flat selection array must hold every option. */
enum {
    XKB_OPT_TOTAL =
        XKB_ARR_N(grp_opts) + XKB_ARR_N(lv2_opts) + XKB_ARR_N(lv3_opts) +
        XKB_ARR_N(ctrl_opts) + XKB_ARR_N(grp_led_opts) + XKB_ARR_N(mod_led_opts) +
        XKB_ARR_N(keypad_opts) + XKB_ARR_N(kpdl_opts) + XKB_ARR_N(caps_opts) +
        XKB_ARR_N(altwin_opts) + XKB_ARR_N(compose_opts) + XKB_ARR_N(compat_opts) +
        XKB_ARR_N(currencysign_opts) + XKB_ARR_N(lv5_opts) + XKB_ARR_N(nbsp_opts) +
        XKB_ARR_N(japan_opts) + XKB_ARR_N(korean_opts) + XKB_ARR_N(esperanto_opts) +
        XKB_ARR_N(solaris_opts) + XKB_ARR_N(terminate_opts) + XKB_ARR_N(fkeys_opts) +
        XKB_ARR_N(custom_opts)
};
typedef char xkb_opt_count_check[(XKB_OPT_TOTAL == XKB_OPT_COUNT) ? 1 : -1];

static int
xkb_code_lookup(const char *code, int *flat_out)
{
    int g, i, flat = 0;

    for (g = 0; g < XKB_GROUP_COUNT; ++g) {
        const struct xkb_option_group *grp = &xkb_groups[g];
        for (i = 0; i < grp->count; ++i, ++flat) {
            if (strcmp(code, grp->opts[i].code) == 0) {
                *flat_out = flat;
                return 1;
            }
        }
    }
    return 0;
}

static size_t
xkb_str_append(char *out, size_t outsz, size_t len, const char *s)
{
    size_t n = strlen(s);
    if (len + n >= outsz)
        n = (outsz > len + 1) ? (outsz - len - 1) : 0;
    if (n > 0) {
        memcpy(out + len, s, n);
        len += n;
    }
    return len;
}

void
xkb_options_build(char *out, size_t outsz, const struct options_xkb *opt)
{
    size_t len = 0;
    int g, i, flat = 0, first = 1;

    out[0] = '\0';
    for (g = 0; g < XKB_GROUP_COUNT; ++g) {
        const struct xkb_option_group *grp = &xkb_groups[g];
        for (i = 0; i < grp->count; ++i, ++flat) {
            if (!opt->opt_sel[flat])
                continue;
            if (!first)
                len = xkb_str_append(out, outsz, len, ",");
            len = xkb_str_append(out, outsz, len, grp->opts[i].code);
            first = 0;
        }
    }
    out[len] = '\0';
}

void
xkb_options_parse(struct options_xkb *opt, const char *str)
{
    int flat;

    for (flat = 0; flat < XKB_OPT_COUNT; ++flat)
        opt->opt_sel[flat] = 0;

    if (!str)
        return;

    while (*str) {
        char code[64];
        size_t n = 0;

        while (*str == ' ' || *str == '\t' || *str == ',')
            ++str;
        if (!*str)
            break;
        while (*str && *str != ',') {
            if (n < sizeof(code) - 1)
                code[n++] = *str;
            ++str;
        }
        while (n > 0 && (code[n - 1] == ' ' || code[n - 1] == '\t'))
            --n;
        code[n] = '\0';
        if (n > 0 && xkb_code_lookup(code, &flat))
            opt->opt_sel[flat] = 1;
    }
}

static void
xkb_checkbox_cell(struct nk_context *ctx, const char *label, int *value, const char *tooltip)
{
    struct nk_rect b = nk_widget_bounds(ctx);
    nk_checkbox_label(ctx, label, value);
    option_tooltip(ctx, b, tooltip);
}

/* Render a multi-option group as checkboxes laid out two per row. */
static void
xkb_checkbox_group(struct nk_context *ctx, const struct xkb_option *opts,
    int count, int *sel)
{
    int i;
    for (i = 0; i < count; ++i) {
        if (i % 2 == 0)
            nk_layout_row_dynamic(ctx, 30, 2);
        xkb_checkbox_cell(ctx, opts[i].name, &sel[i], opts[i].tooltip ? opts[i].tooltip : opts[i].name);
    }
}

/* Render a single-choice group as a full-width drop-down, "[None]" = unset. */
static void
xkb_dropdown_group(struct nk_context *ctx, const struct xkb_option *opts,
    int count, int *sel)
{
    const char *items[64];
    struct nk_rect b;
    int sel_index = 0, i;

    for (i = 0; i < count; ++i) {
        items[i + 1] = opts[i].name;
        if (sel[i])
            sel_index = i + 1;
    }
    items[0] = "[None]";

    nk_layout_row_dynamic(ctx, 30, 1);
    b = nk_widget_bounds(ctx);
    nk_combobox(ctx, items, count + 1, &sel_index, 25,
        nk_vec2(nk_widget_width(ctx), 200));
    option_tooltip(ctx, b, items[sel_index]);

    for (i = 0; i < count; ++i)
        sel[i] = (sel_index == i + 1);
}

/* Resolve the default XKB base directory: "<launchx.exe dir>\xkbdata".
   ming64x.exe uses this same folder when -xkbdir is empty (winFixupPaths()
   sets XkbBaseDirectory = "<basedir>xkbdata"), and launchx.exe starts it from
   its own directory, so the two executables are co-located. */
static int
xkb_dir_default(char *out, size_t outsz)
{
    char dir[MAX_PATH];
    DWORD len;
    char *slash;
    HANDLE h;
    DWORD n;

    out[0] = '\0';
    len = GetModuleFileNameA(NULL, dir, sizeof dir);
    if (len == 0 || len >= sizeof dir)
        return -1;
    slash = strrchr(dir, '\\');
    if (slash)
        slash[1] = '\0';        /* keep trailing backslash */
    else {
        dir[0] = '.';
        dir[1] = '\\';
        dir[2] = '\0';
    }

    snprintf(out, outsz, "%sxkbdata", dir);

    h = CreateFileA(dir, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        char real[MAX_PATH];
        n = GetFinalPathNameByHandleA(h, real, (DWORD) sizeof real,
                                      FILE_NAME_NORMALIZED);
        CloseHandle(h);
        if (n > 0 && n < sizeof real) {
            if (!strncmp(real, "\\\\?\\", 4))
                memmove(real, real + 4, strlen(real + 4) + 1);
            snprintf(out, outsz, "%s\\xkbdata", real);
        }
    }
    return 0;
}

void
tab_xkb(struct nk_context *ctx, struct options_xkb *opt)
{
    int i, g, flat = 0;

    heading(ctx, "International keyboard layout");

    for (i = 0; i < 3; i++) {
        int disabled = 0;
        int j, k;
        const char *vitems[VARIANT_COUNT + 1];
        const char *vcodes[VARIANT_COUNT + 1];
        int vcount = 1;
        int vsel = 0;
        const char *layout = layout_codes[opt->layout_sel[i]];

        for (j = 0; j < i; j++)
            if (opt->layout_sel[j] == 0)
                disabled = 1;

        vitems[0] = "[None]";
        vcodes[0] = "";
        for (k = 0; k < VARIANT_COUNT; k++) {
            if (layout[0] != '\0' && strcmp(variants[k].layout, layout) == 0) {
                vitems[vcount] = variants[k].name;
                vcodes[vcount] = variants[k].code;
                vcount++;
            }
        }
        for (k = 1; k < vcount; k++) {
            if (strcmp(opt->xkbvariant[i], vcodes[k]) == 0) {
                vsel = k;
                break;
            }
        }

        if (disabled) {
            opt->layout_sel[i] = 0;
            opt->xkbvariant[i][0] = '\0';
            nk_widget_disable_begin(ctx);
        }

        combobox_2options(ctx,
            (i == 0) ? "Layout (-xkblayout)" :
            (i == 1) ? "Second level" : "Third level",
            i != 0,
            layout_items, LAYOUT_COUNT, &opt->layout_sel[i],
            vitems, vcount, &vsel,
            "Keyboard layout and its variant. [Auto-detect] matches your current\n"
            "Windows layout. Multiple non-Auto layouts are joined with commas into\n"
            "-xkblayout (e.g. us,ru,de); variants join positionally into -xkbvariant\n"
            "(e.g. dvorak,,).");

        strncpy(opt->xkbvariant[i], vcodes[vsel], 63);
        opt->xkbvariant[i][63] = '\0';

        if (disabled)
            nk_widget_disable_end(ctx);
    }

    text_option(ctx, "Model (-xkbmodel)", opt->xkbmodel, (int)sizeof(opt->xkbmodel), "Keyboard model to load (default pc105).");

    for (g = 0; g < XKB_GROUP_COUNT; ++g) {
        const struct xkb_option_group *grp = &xkb_groups[g];
        int *sel = &opt->opt_sel[flat];

        heading(ctx, grp->title);
        if (grp->count == 1)
            checkbox_option(ctx, grp->opts[0].name, sel, grp->opts[0].tooltip ? grp->opts[0].tooltip : grp->opts[0].name);
        else if (grp->multiple)
            xkb_checkbox_group(ctx, grp->opts, grp->count, sel);
        else
            xkb_dropdown_group(ctx, grp->opts, grp->count, sel);

        flat += grp->count;
    }

    text_option(ctx, "Rules (-xkbrules)", opt->xkbrules, (int)sizeof(opt->xkbrules), "XKB rules file to use (default xorg).");

    {
        char path[512];
        if (xkb_dir_default(path, sizeof path))
            path[0] = '\0';
        readonly_option(ctx, "Default XKB base directory", path,
            "Folder the server searches for XKB data when -xkbdir is empty.\n"
            "Drop custom component files here, e.g. types/custom or symbols/custom.");
    }

    text_option(ctx, "Override base directory (-xkbdir)", opt->xkbdir, (int)sizeof(opt->xkbdir), "Base directory for XKB keyboard layout files.");
}
