#include "chwp/model/document.hpp"

// Header of a new Hangul 2024 document: the 23 default styles and the paragraph/character shapes, outline numbering
// and tab definitions they use. Generated from a document Hangul saved right after File > New.

namespace chwp::model {

void addDefaultStyleHeader(Document& doc) {
    doc.header.borderFills.clear();
    BorderFill none;
    none.id = 1;
    none.left.type = none.right.type = none.top.type = none.bottom.type = "NONE";
    doc.header.borderFills.push_back(none);
    BorderFill clear = none;
    clear.id = 2;
    clear.faceColor = "none";
    clear.hatchColor = "#999999";
    doc.header.borderFills.push_back(clear);
    doc.header.borderFillCount = doc.header.borderFills.size();
    doc.header.charShapes.clear();
    doc.header.paraShapes.clear();
    doc.header.styles.clear();
    doc.header.tabDefs.clear();
    doc.header.numberings.clear();
    {
        CharShape cs;
        cs.id = 0;
        cs.fontRef.fill(1);
        cs.ratio.fill(100);
        cs.relSize.fill(100);
        cs.underlineColor = 0x000000u;
        cs.strikeColor = 0x000000u;
        cs.shadowColor = 0xC0C0C0u;
        cs.borderFillId = 2;
        doc.header.charShapes.push_back(cs);
    }
    {
        CharShape cs;
        cs.id = 1;
        cs.ratio.fill(100);
        cs.relSize.fill(100);
        cs.underlineColor = 0x000000u;
        cs.strikeColor = 0x000000u;
        cs.shadowColor = 0xC0C0C0u;
        cs.borderFillId = 2;
        doc.header.charShapes.push_back(cs);
    }
    {
        CharShape cs;
        cs.id = 2;
        cs.height = 900;
        cs.ratio.fill(100);
        cs.relSize.fill(100);
        cs.underlineColor = 0x000000u;
        cs.strikeColor = 0x000000u;
        cs.shadowColor = 0xC0C0C0u;
        cs.borderFillId = 2;
        doc.header.charShapes.push_back(cs);
    }
    {
        CharShape cs;
        cs.id = 3;
        cs.height = 900;
        cs.fontRef.fill(1);
        cs.ratio.fill(100);
        cs.relSize.fill(100);
        cs.underlineColor = 0x000000u;
        cs.strikeColor = 0x000000u;
        cs.shadowColor = 0xC0C0C0u;
        cs.borderFillId = 2;
        doc.header.charShapes.push_back(cs);
    }
    {
        CharShape cs;
        cs.id = 4;
        cs.height = 900;
        cs.ratio.fill(100);
        cs.spacing.fill(-5);
        cs.relSize.fill(100);
        cs.underlineColor = 0x000000u;
        cs.strikeColor = 0x000000u;
        cs.shadowColor = 0xC0C0C0u;
        cs.borderFillId = 2;
        doc.header.charShapes.push_back(cs);
    }
    {
        CharShape cs;
        cs.id = 5;
        cs.height = 1600;
        cs.ratio.fill(100);
        cs.relSize.fill(100);
        cs.textColor = 0x2E74B5u;
        cs.underlineColor = 0x000000u;
        cs.strikeColor = 0x000000u;
        cs.shadowColor = 0xC0C0C0u;
        cs.borderFillId = 2;
        doc.header.charShapes.push_back(cs);
    }
    {
        CharShape cs;
        cs.id = 6;
        cs.height = 1100;
        cs.ratio.fill(100);
        cs.relSize.fill(100);
        cs.underlineColor = 0x000000u;
        cs.strikeColor = 0x000000u;
        cs.shadowColor = 0xC0C0C0u;
        cs.borderFillId = 2;
        doc.header.charShapes.push_back(cs);
    }
    {
        ParaShape ps;
        ps.id = 0;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 1;
        ps.marginLeft = 1500;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 2;
        ps.marginLeft = 1000;
        ps.condense = 20;
        ps.headingType = "OUTLINE";
        ps.tabDefId = 1;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 3;
        ps.marginLeft = 2000;
        ps.condense = 20;
        ps.headingType = "OUTLINE";
        ps.headingLevel = 1;
        ps.tabDefId = 1;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 4;
        ps.marginLeft = 3000;
        ps.condense = 20;
        ps.headingType = "OUTLINE";
        ps.headingLevel = 2;
        ps.tabDefId = 1;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 5;
        ps.marginLeft = 4000;
        ps.condense = 20;
        ps.headingType = "OUTLINE";
        ps.headingLevel = 3;
        ps.tabDefId = 1;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 6;
        ps.marginLeft = 5000;
        ps.condense = 20;
        ps.headingType = "OUTLINE";
        ps.headingLevel = 4;
        ps.tabDefId = 1;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 7;
        ps.marginLeft = 6000;
        ps.condense = 20;
        ps.headingType = "OUTLINE";
        ps.headingLevel = 5;
        ps.tabDefId = 1;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 8;
        ps.marginLeft = 7000;
        ps.condense = 20;
        ps.headingType = "OUTLINE";
        ps.headingLevel = 6;
        ps.tabDefId = 1;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 9;
        ps.lineSpacing = 150;
        ps.breakNonLatinWord = "BREAK_WORD";
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 10;
        ps.indent = -1310;
        ps.lineSpacing = 130;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 11;
        ps.align = "LEFT";
        ps.lineSpacing = 130;
        ps.breakNonLatinWord = "BREAK_WORD";
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 12;
        ps.align = "LEFT";
        ps.spacingPrev = 1200;
        ps.spacingNext = 300;
        ps.breakNonLatinWord = "BREAK_WORD";
        ps.condense = 20;
        ps.tabDefId = 1;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 13;
        ps.align = "LEFT";
        ps.spacingNext = 700;
        ps.breakNonLatinWord = "BREAK_WORD";
        ps.tabDefId = 2;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 14;
        ps.align = "LEFT";
        ps.marginLeft = 1100;
        ps.spacingNext = 700;
        ps.breakNonLatinWord = "BREAK_WORD";
        ps.tabDefId = 2;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 15;
        ps.align = "LEFT";
        ps.marginLeft = 2200;
        ps.spacingNext = 700;
        ps.breakNonLatinWord = "BREAK_WORD";
        ps.tabDefId = 2;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 16;
        ps.marginLeft = 9000;
        ps.tabDefId = 1;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 17;
        ps.marginLeft = 10000;
        ps.tabDefId = 1;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 18;
        ps.marginLeft = 8000;
        ps.tabDefId = 1;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        ParaShape ps;
        ps.id = 19;
        ps.spacingNext = 800;
        ps.lineSpacing = 150;
        ps.borderFillId = 2;
        ps.snapToGrid = true;
        doc.header.paraShapes.push_back(ps);
    }
    {
        Style st;
        st.id = 0;
        st.name = "바탕글";
        st.engName = "Normal";
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 1;
        st.name = "본문";
        st.engName = "Body";
        st.paraShapeId = 1;
        st.nextStyleId = 1;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 2;
        st.name = "개요 1";
        st.engName = "Outline 1";
        st.paraShapeId = 2;
        st.nextStyleId = 2;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 3;
        st.name = "개요 2";
        st.engName = "Outline 2";
        st.paraShapeId = 3;
        st.nextStyleId = 3;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 4;
        st.name = "개요 3";
        st.engName = "Outline 3";
        st.paraShapeId = 4;
        st.nextStyleId = 4;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 5;
        st.name = "개요 4";
        st.engName = "Outline 4";
        st.paraShapeId = 5;
        st.nextStyleId = 5;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 6;
        st.name = "개요 5";
        st.engName = "Outline 5";
        st.paraShapeId = 6;
        st.nextStyleId = 6;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 7;
        st.name = "개요 6";
        st.engName = "Outline 6";
        st.paraShapeId = 7;
        st.nextStyleId = 7;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 8;
        st.name = "개요 7";
        st.engName = "Outline 7";
        st.paraShapeId = 8;
        st.nextStyleId = 8;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 9;
        st.name = "개요 8";
        st.engName = "Outline 8";
        st.paraShapeId = 18;
        st.nextStyleId = 9;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 10;
        st.name = "개요 9";
        st.engName = "Outline 9";
        st.paraShapeId = 16;
        st.nextStyleId = 10;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 11;
        st.name = "개요 10";
        st.engName = "Outline 10";
        st.paraShapeId = 17;
        st.nextStyleId = 11;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 12;
        st.type = "CHAR";
        st.name = "쪽 번호";
        st.engName = "Page Number";
        st.charShapeId = 1;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 13;
        st.type = "CHAR";
        st.name = "줄 번호";
        st.engName = "Line Number";
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 14;
        st.name = "머리말";
        st.engName = "Header";
        st.paraShapeId = 9;
        st.charShapeId = 2;
        st.nextStyleId = 14;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 15;
        st.name = "각주";
        st.engName = "Footnote";
        st.paraShapeId = 10;
        st.charShapeId = 3;
        st.nextStyleId = 15;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 16;
        st.name = "미주";
        st.engName = "Endnote";
        st.paraShapeId = 10;
        st.charShapeId = 3;
        st.nextStyleId = 16;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 17;
        st.name = "메모";
        st.engName = "Memo";
        st.paraShapeId = 11;
        st.charShapeId = 4;
        st.nextStyleId = 17;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 18;
        st.name = "차례 제목";
        st.engName = "TOC Heading";
        st.paraShapeId = 12;
        st.charShapeId = 5;
        st.nextStyleId = 18;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 19;
        st.name = "차례 1";
        st.engName = "TOC 1";
        st.paraShapeId = 13;
        st.charShapeId = 6;
        st.nextStyleId = 19;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 20;
        st.name = "차례 2";
        st.engName = "TOC 2";
        st.paraShapeId = 14;
        st.charShapeId = 6;
        st.nextStyleId = 20;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 21;
        st.name = "차례 3";
        st.engName = "TOC 3";
        st.paraShapeId = 15;
        st.charShapeId = 6;
        st.nextStyleId = 21;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        Style st;
        st.id = 22;
        st.name = "캡션";
        st.engName = "Caption";
        st.paraShapeId = 19;
        st.nextStyleId = 22;
        st.langId = 1042;
        doc.header.styles.push_back(st);
    }
    {
        TabDef tab;
        tab.id = 0;
        tab.autoTabLeft = false;
        tab.autoTabRight = false;
        doc.header.tabDefs.push_back(tab);
    }
    {
        TabDef tab;
        tab.id = 1;
        tab.autoTabLeft = true;
        tab.autoTabRight = false;
        doc.header.tabDefs.push_back(tab);
    }
    {
        TabDef tab;
        tab.id = 2;
        tab.autoTabLeft = false;
        tab.autoTabRight = true;
        doc.header.tabDefs.push_back(tab);
    }
    {
        Numbering num;
        num.id = 1;
        num.start = 0;
        {
            ParaHead head;
            head.format = "^1.";
            num.levels.push_back(head);
        }
        {
            ParaHead head;
            head.level = 2;
            head.numFormat = "HANGUL_SYLLABLE";
            head.format = "^2.";
            num.levels.push_back(head);
        }
        {
            ParaHead head;
            head.level = 3;
            head.format = "^3)";
            num.levels.push_back(head);
        }
        {
            ParaHead head;
            head.level = 4;
            head.numFormat = "HANGUL_SYLLABLE";
            head.format = "^4)";
            num.levels.push_back(head);
        }
        {
            ParaHead head;
            head.level = 5;
            head.format = "(^5)";
            num.levels.push_back(head);
        }
        {
            ParaHead head;
            head.level = 6;
            head.numFormat = "HANGUL_SYLLABLE";
            head.format = "(^6)";
            num.levels.push_back(head);
        }
        {
            ParaHead head;
            head.level = 7;
            head.numFormat = "CIRCLED_DIGIT";
            head.format = "^7";
            num.levels.push_back(head);
        }
        {
            ParaHead head;
            head.level = 8;
            head.numFormat = "CIRCLED_HANGUL_SYLLABLE";
            head.format = "^8";
            num.levels.push_back(head);
        }
        {
            ParaHead head;
            head.level = 9;
            head.numFormat = "HANGUL_JAMO";
            num.levels.push_back(head);
        }
        {
            ParaHead head;
            head.level = 10;
            head.numFormat = "ROMAN_SMALL";
            num.levels.push_back(head);
        }
        doc.header.numberings.push_back(num);
    }
}

}  // namespace chwp::model
