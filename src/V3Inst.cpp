// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Add temporaries, such as for inst nodes
//
// Code available from: http://www.veripool.org/verilator
//
//*************************************************************************
//
// Copyright 2003-2016 by Wilson Snyder.  This program is free software; you can
// redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
//
// Verilator is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
//*************************************************************************
// V3Inst's Transformations:
//
// Each module:
//	Pins:
//	    Create a wire assign to interconnect to submodule
//
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"
#include <cstdio>
#include <cstdarg>
#include <unistd.h>
#include <algorithm>

#include "V3Global.h"
#include "V3Inst.h"
#include "V3Ast.h"
#include "V3Changed.h"
#include "V3Const.h"

//######################################################################
// Inst state, as a visitor of each AstNode

class InstVisitor : public AstNVisitor {
private:
    // NODE STATE
    // Cleared each Cell:
    //  AstPin::user1p()	-> bool.  True if created assignment already
    AstUser1InUse	m_inuser1;

    // STATE
    AstNodeModule*	m_modp;		// Current module
    AstCell*	m_cellp;	// Current cell

    static int debug() {
	static int level = -1;
	if (VL_UNLIKELY(level < 0)) level = v3Global.opt.debugSrcLevel(__FILE__);
	return level;
    }
    //int m_debug;  int debug() { return m_debug; }

    // VISITORS
    virtual void visit(AstNodeModule* nodep, AstNUser*) {
	UINFO(4," MOD   "<<nodep<<endl);
	//if (nodep->name() == "t_chg") m_debug = 9; else m_debug=0;
	m_modp = nodep;
	nodep->iterateChildren(*this);
	m_modp = NULL;
    }
    virtual void visit(AstCell* nodep, AstNUser*) {
	UINFO(4,"  CELL   "<<nodep<<endl);
	m_cellp = nodep;
	//VV*****  We reset user1p() on each cell!!!
	AstNode::user1ClearTree();
	nodep->iterateChildren(*this);
	m_cellp = NULL;
    }
    virtual void visit(AstPin* nodep, AstNUser*) {
	// PIN(p,expr) -> ASSIGNW(VARXREF(p),expr)    (if sub's input)
	//	      or  ASSIGNW(expr,VARXREF(p))    (if sub's output)
	UINFO(4,"   PIN  "<<nodep<<endl);
	if (!nodep->exprp()) return; // No-connect
	if (debug()>=9) nodep->dumpTree(cout,"  Pin_oldb: ");
	if (nodep->modVarp()->isOutOnly() && nodep->exprp()->castConst())
	    nodep->v3error("Output port is connected to a constant pin, electrical short");
	// Use user1p on the PIN to indicate we created an assign for this pin
	if (!nodep->user1SetOnce()) {
	    // Simplify it
	    V3Inst::pinReconnectSimple(nodep, m_cellp, m_modp, false);
	    // Make a ASSIGNW (expr, pin)
	    AstNode*  exprp  = nodep->exprp()->cloneTree(false);
	    if (exprp->width() != nodep->modVarp()->width())
		nodep->v3fatalSrc("Width mismatch, should have been handled in pinReconnectSimple\n");
	    if (nodep->modVarp()->isInout()) {
		nodep->v3fatalSrc("Unsupported: Verilator is a 2-state simulator");
	    } else if (nodep->modVarp()->isOutput()) {
		AstNode* rhsp = new AstVarXRef (exprp->fileline(), nodep->modVarp(), m_cellp->name(), false);
		AstAssignW* assp = new AstAssignW (exprp->fileline(), exprp, rhsp);
		m_modp->addStmtp(assp);
	    } else if (nodep->modVarp()->isInput()) {
		// Don't bother moving constants now,
		// we'll be pushing the const down to the cell soon enough.
		AstNode* assp = new AstAssignW
		    (exprp->fileline(),
		     new AstVarXRef(exprp->fileline(), nodep->modVarp(), m_cellp->name(), true),
		     exprp);
		m_modp->addStmtp(assp);
		if (debug()>=9) assp->dumpTree(cout,"     _new: ");
	    } else if (nodep->modVarp()->isIfaceRef()
		       || (nodep->modVarp()->subDTypep()->castUnpackArrayDType()
			   && nodep->modVarp()->subDTypep()->castUnpackArrayDType()->subDTypep()->castIfaceRefDType())) {
		// Create an AstAssignVarScope for Vars to Cells so we can link with their scope later
		AstNode* lhsp = new AstVarXRef (exprp->fileline(), nodep->modVarp(), m_cellp->name(), false);
		AstVarRef* refp = exprp->castVarRef();
		AstVarXRef* xrefp = exprp->castVarXRef();
		if (!refp && !xrefp) exprp->v3fatalSrc("Interfaces: Pin is not connected to a VarRef or VarXRef");
		AstAssignVarScope* assp = new AstAssignVarScope(exprp->fileline(), lhsp, exprp);
		m_modp->addStmtp(assp);
	    } else {
		nodep->v3error("Assigned pin is neither input nor output");
	    }
	}

	// We're done with the pin
	nodep->unlinkFrBack()->deleteTree(); VL_DANGLING(nodep);
    }

    virtual void visit(AstUdpTable* nodep, AstNUser*) {
	if (!v3Global.opt.bboxUnsup()) {
	    // If we support primitives, update V3Undriven to remove special case
	    nodep->v3error("Unsupported: Verilog 1995 UDP Tables.  Use --bbox-unsup to ignore tables.");
	}
    }

    // Save some time
    virtual void visit(AstNodeAssign*, AstNUser*) {}
    virtual void visit(AstAlways*, AstNUser*) {}

    //--------------------
    // Default: Just iterate
    virtual void visit(AstNode* nodep, AstNUser*) {
	nodep->iterateChildren(*this);
    }
public:
    // CONSTUCTORS
    explicit InstVisitor(AstNode* nodep) {
	m_modp=NULL;
	m_cellp=NULL;
	//
	nodep->accept(*this);
    }
    virtual ~InstVisitor() {}
};

//######################################################################

class InstDeVisitor : public AstNVisitor {
    // Find all cells with arrays, and convert to non-arrayed
private:
    // STATE
    AstRange*	m_cellRangep;	// Range for arrayed instantiations, NULL for normal instantiations
    int		m_instNum;	// Current instantiation number
    int		m_instLsb;	// Current instantiation number

    static int debug() {
	static int level = -1;
	if (VL_UNLIKELY(level < 0)) level = v3Global.opt.debugSrcLevel(__FILE__);
	return level;
    }

    // VISITORS
    virtual void visit(AstCell* nodep, AstNUser*) {
	if (nodep->rangep()) {
	    m_cellRangep = nodep->rangep();
	    UINFO(4,"  CELL   "<<nodep<<endl);

	    AstVar* ifaceVarp = nodep->nextp()->castVar();
	    bool isIface = ifaceVarp
		&& ifaceVarp->dtypep()->castUnpackArrayDType()
		&& ifaceVarp->dtypep()->castUnpackArrayDType()->subDTypep()->castIfaceRefDType();

	    // Make all of the required clones
	    m_instLsb = m_cellRangep->lsbConst();
	    for (m_instNum = m_instLsb; m_instNum<=m_cellRangep->msbConst(); m_instNum++) {
		AstCell* newp = nodep->cloneTree(false);
		nodep->addNextHere(newp);
		// Remove ranging and fix name
		newp->rangep()->unlinkFrBack()->deleteTree();
		// Somewhat illogically, we need to rename the orignal name of the cell too.
		// as that is the name users expect for dotting
		// The spec says we add [x], but that won't work in C...
		newp->name(newp->name()+"__BRA__"+cvtToStr(m_instNum)+"__KET__");
		newp->origName(newp->origName()+"__BRA__"+cvtToStr(m_instNum)+"__KET__");

		// If this AstCell is actually an interface instantiation, let's ensure we also clone
		// the IfaceRef.
		if (isIface) {
		    AstUnpackArrayDType* arrdtype = ifaceVarp->dtypep()->castUnpackArrayDType();
		    AstIfaceRefDType* origIfaceRefp = arrdtype->subDTypep()->castIfaceRefDType();
		    origIfaceRefp->cellp(NULL);
		    AstVar* varNewp = ifaceVarp->cloneTree(false);
		    AstIfaceRefDType* ifaceRefp = arrdtype->subDTypep()->castIfaceRefDType()->cloneTree(false);
		    arrdtype->addNextHere(ifaceRefp);
		    ifaceRefp->cellp(newp);
		    ifaceRefp->cellName(newp->name());
		    varNewp->name(varNewp->name() + "__BRA__" + cvtToStr(m_instNum) + "__KET__");
		    varNewp->origName(varNewp->origName() + "__BRA__" + cvtToStr(m_instNum) + "__KET__");
		    varNewp->dtypep(ifaceRefp);
		    newp->addNextHere(varNewp);
		    if (debug()==9) { varNewp->dumpTree(cout, "newintf: "); cout << endl; }
		}
		// Fixup pins
		newp->pinsp()->iterateAndNext(*this);
		if (debug()==9) { newp->dumpTree(cout,"newcell: "); cout<<endl; }
	    }

	    // Done.  Delete original
	    m_cellRangep=NULL;
	    if (isIface) {
		ifaceVarp->unlinkFrBack(); pushDeletep(ifaceVarp); VL_DANGLING(ifaceVarp);
	    }
	    nodep->unlinkFrBack(); pushDeletep(nodep); VL_DANGLING(nodep);
	}
	nodep->iterateChildren(*this);
    }

    virtual void visit(AstVar* nodep, AstNUser*) {
	bool isIface = nodep->dtypep()->castUnpackArrayDType()
	    && nodep->dtypep()->castUnpackArrayDType()->subDTypep()->castIfaceRefDType();
	if (isIface) {
	    AstUnpackArrayDType* arrdtype = nodep->dtypep()->castUnpackArrayDType();
	    AstNode* prev = NULL;
	    for (int i = arrdtype->lsb(); i <= arrdtype->msb(); ++i) {
		AstVar* varNewp = nodep->cloneTree(false);
		AstIfaceRefDType* ifaceRefp = arrdtype->subDTypep()->castIfaceRefDType()->cloneTree(false);
		arrdtype->addNextHere(ifaceRefp);
		ifaceRefp->cellp(NULL);
		varNewp->name(varNewp->name() + "__BRA__" + cvtToStr(i) + "__KET__");
		varNewp->origName(varNewp->origName() + "__BRA__" + cvtToStr(i) + "__KET__");
		varNewp->dtypep(ifaceRefp);
		if (!prev) {
		    prev = varNewp;
		} else {
		    prev->addNextHere(varNewp);
		}
	    }
	    nodep->addNextHere(prev);
	    if (debug()==9) { prev->dumpTree(cout, "newintf: "); cout << endl; }
	}
	nodep->iterateChildren(*this);
    }

    virtual void visit(AstPin* nodep, AstNUser*) {
	// Any non-direct pins need reconnection with a part-select
	if (!nodep->exprp()) return; // No-connect
	if (m_cellRangep) {
	    UINFO(4,"   PIN  "<<nodep<<endl);
	    int pinwidth = nodep->modVarp()->width();
	    int expwidth = nodep->exprp()->width();
	    pair<uint32_t,uint32_t> pinDim = nodep->modVarp()->dtypep()->dimensions(false);
	    pair<uint32_t,uint32_t> expDim = nodep->exprp()->dtypep()->dimensions(false);
	    UINFO(4,"   PINVAR  "<<nodep->modVarp()<<endl);
	    UINFO(4,"   EXP     "<<nodep->exprp()<<endl);
	    UINFO(4,"   pinwidth ew="<<expwidth<<" pw="<<pinwidth
		  <<"  ed="<<expDim.first<<","<<expDim.second
		  <<"  pd="<<pinDim.first<<","<<pinDim.second<<endl);
	    if (expDim.first == pinDim.first && expDim.second == pinDim.second+1) {
		// Connection to array, where array dimensions match the instant dimension
		AstNode* exprp = nodep->exprp()->unlinkFrBack();
		exprp = new AstArraySel (exprp->fileline(), exprp,
					 (m_instNum-m_instLsb));
		nodep->exprp(exprp);
	    } else if (expwidth == pinwidth) {
		// NOP: Arrayed instants: widths match so connect to each instance
	    } else if (expwidth == pinwidth*m_cellRangep->elementsConst()) {
		// Arrayed instants: one bit for each of the instants (each assign is 1 pinwidth wide)
		AstNode* exprp = nodep->exprp()->unlinkFrBack();
		bool inputPin = nodep->modVarp()->isInput();
		if (!inputPin && !exprp->castVarRef()
		    && !exprp->castConcat()  // V3Const will collapse the SEL with the one we're about to make
		    && !exprp->castSel()) {  // V3Const will collapse the SEL with the one we're about to make
		    nodep->v3error("Unsupported: Per-bit array instantiations with output connections to non-wires.");
		    // Note spec allows more complicated matches such as slices and such
		}
		exprp = new AstSel (exprp->fileline(), exprp,
				    pinwidth*(m_instNum-m_instLsb),
				    pinwidth);
		nodep->exprp(exprp);
	    } else {
		nodep->v3fatalSrc("Width mismatch; V3Width should have errored out.");
	    }
	} else if (AstArraySel* arrselp = nodep->exprp()->castArraySel()) {
	    if (AstUnpackArrayDType* arrp = arrselp->lhsp()->dtypep()->castUnpackArrayDType()) {
		if (!arrp->subDTypep()->castIfaceRefDType())
		    return;

		V3Const::constifyParamsEdit(arrselp->rhsp());
		AstConst* constp = arrselp->rhsp()->castConst();
		if (!constp) {
		    nodep->v3error("Unsupported: Non-constant index when passing interface to module");
		    return;
		}
		string index = AstNode::encodeNumber(constp->toSInt());
		AstVarRef* varrefp = arrselp->lhsp()->castVarRef();
		AstVarXRef* newp = new AstVarXRef(nodep->fileline(),varrefp->name () + "__BRA__" + index  + "__KET__", "", true);
		newp->dtypep(nodep->modVarp()->dtypep());
		newp->packagep(varrefp->packagep());
		arrselp->addNextHere(newp);
		arrselp->unlinkFrBack()->deleteTree();
	    }
	} else {
	    AstVar* pinVarp = nodep->modVarp();
	    AstUnpackArrayDType* pinArrp = pinVarp->dtypep()->castUnpackArrayDType();
	    if (!pinArrp || !pinArrp->subDTypep()->castIfaceRefDType())
		return;
	    AstNode* prevp = NULL;
	    AstNode* prevPinp = NULL;
	    // Clone the var referenced by the pin, and clone each var referenced by the varref
	    // Clone pin varp:
	    for (int i = pinArrp->lsb(); i <= pinArrp->msb(); ++i) {
		AstVar* varNewp = pinVarp->cloneTree(false);
		AstIfaceRefDType* ifaceRefp = pinArrp->subDTypep()->castIfaceRefDType();
		ifaceRefp->cellp(NULL);
		varNewp->name(varNewp->name() + "__BRA__" + cvtToStr(i) + "__KET__");
		varNewp->origName(varNewp->origName() + "__BRA__" + cvtToStr(i) + "__KET__");
		varNewp->dtypep(ifaceRefp);
		if (!prevp) {
		    prevp = varNewp;
		} else {
		    prevp->addNextHere(varNewp);
		}
		// Now also clone the pin itself and update its varref
		AstPin* newp = nodep->cloneTree(false);
		newp->modVarp(varNewp);
		newp->name(newp->name() + "__BRA__" + cvtToStr(i) + "__KET__");
		// And replace exprp with a new varxref
		AstVarRef* varrefp = newp->exprp()->castVarRef();
		string newname = varrefp->name () + "__BRA__" + cvtToStr(i) + "__KET__";
		AstVarXRef* newVarXRefp = new AstVarXRef (nodep->fileline(), newname, "", true);
		newVarXRefp->varp(newp->modVarp());
		newVarXRefp->dtypep(newp->modVarp()->dtypep());
		newp->exprp()->unlinkFrBack()->deleteTree();
		newp->exprp(newVarXRefp);
		if (!prevPinp) {
		    prevPinp = newp;
		} else {
		    prevPinp->addNextHere(newp);
		}
	    }
	    pinVarp->replaceWith(prevp);
	    nodep->replaceWith(prevPinp);
	    pushDeletep(pinVarp);
	    pushDeletep(nodep);

	}
    }

    // Save some time
    virtual void visit(AstNodeMath*, AstNUser*) {}
    //--------------------
    // Default: Just iterate
    virtual void visit(AstNode* nodep, AstNUser*) {
	nodep->iterateChildren(*this);
    }
public:
    // CONSTUCTORS
    explicit InstDeVisitor(AstNode* nodep) {
	m_cellRangep=NULL;
	m_instNum=0;
	m_instLsb=0;
	//
	nodep->accept(*this);
    }
    virtual ~InstDeVisitor() {}
};

//######################################################################
// Inst static function

class InstStatic {
private:
    static int debug() {
	static int level = -1;
	if (VL_UNLIKELY(level < 0)) level = v3Global.opt.debugSrcLevel(__FILE__);
	return level;
    }
    InstStatic() {} // Static class

    static AstNode* extendOrSel(FileLine* fl, AstNode* rhsp, AstNode* cmpWidthp) {
	if (cmpWidthp->width() > rhsp->width()) {
	    rhsp = (rhsp->isSigned()
		    ? (new AstExtendS(fl, rhsp))->castNode()
		    : (new AstExtend (fl, rhsp))->castNode());
	    rhsp->dtypeFrom(cmpWidthp);  // Need proper widthMin, which may differ from AstSel created above
	} else if (cmpWidthp->width() < rhsp->width()) {
	    rhsp = new AstSel (fl, rhsp, 0, cmpWidthp->width());
	    rhsp->dtypeFrom(cmpWidthp);  // Need proper widthMin, which may differ from AstSel created above
	}
	// else don't change dtype, as might be e.g. array of something
	return rhsp;
    }

public:
    static AstAssignW* pinReconnectSimple(AstPin* pinp, AstCell* cellp, AstNodeModule*,
					  bool forTristate, bool alwaysCvt) {
	// If a pin connection is "simple" leave it as-is
	// Else create a intermediate wire to perform the interconnect
	// Return the new assignment, if one was made
	// Note this module calles cloneTree() via new AstVar

	AstVar* pinVarp = pinp->modVarp();
	AstVarRef* connectRefp = pinp->exprp()->castVarRef();
	AstVarXRef* connectXRefp = pinp->exprp()->castVarXRef();
	AstBasicDType* pinBasicp = pinVarp->dtypep()->basicp();  // Maybe NULL
	AstBasicDType* connBasicp = NULL;
	AstAssignW* assignp = NULL;
	if (connectRefp) connBasicp = connectRefp->varp()->dtypep()->basicp();
	//
	if (!alwaysCvt
	    && connectRefp
	    && connectRefp->varp()->dtypep()->sameTree(pinVarp->dtypep())
	    && !connectRefp->varp()->isSc()) { // Need the signal as a 'shell' to convert types
	    // Done. Same data type
	} else if (!alwaysCvt
		   && connectRefp
		   && connectRefp->varp()->isIfaceRef()) {
	    // Done. Interface
	} else if (!alwaysCvt
		   && connectXRefp
		   && connectXRefp->varp()
		   && connectXRefp->varp()->isIfaceRef()) {
	} else if (!alwaysCvt
		   && connBasicp
		   && pinBasicp
		   && connBasicp->width() == pinBasicp->width()
		   && connBasicp->lsb() == pinBasicp->lsb()
		   && !connectRefp->varp()->isSc()	// Need the signal as a 'shell' to convert types
		   && connBasicp->width() == pinVarp->width()
		   && 1) {
	    // Done. One to one interconnect won't need a temporary variable.
	} else if (!alwaysCvt && !forTristate && pinp->exprp()->castConst()) {
	    // Done. Constant.
	} else {
	    // Make a new temp wire
	    //if (1||debug()>=9) { pinp->dumpTree(cout,"-in_pin:"); }
	    AstNode* pinexprp = pinp->exprp()->unlinkFrBack();
	    string newvarname = ((string)(pinVarp->isOutput() ? "__Vcellout" : "__Vcellinp")
				 +(forTristate?"t":"")  // Prevent name conflict if both tri & non-tri add signals
				 +"__"+cellp->name()+"__"+pinp->name());
	    AstVar* newvarp = new AstVar (pinVarp->fileline(), AstVarType::MODULETEMP, newvarname, pinVarp);
	    // Important to add statement next to cell, in case there is a generate with same named cell
	    cellp->addNextHere(newvarp);
	    if (pinVarp->isInout()) {
		pinVarp->v3fatalSrc("Unsupported: Inout connections to pins must be direct one-to-one connection (without any expression)");
	    } else if (pinVarp->isOutput()) {
		// See also V3Inst
		AstNode* rhsp = new AstVarRef(pinp->fileline(), newvarp, false);
		UINFO(5,"pinRecon width "<<pinVarp->width()<<" >? "<<rhsp->width()<<" >? "<<pinexprp->width()<<endl);
		rhsp = extendOrSel (pinp->fileline(), rhsp, pinVarp);
		pinp->exprp(new AstVarRef (newvarp->fileline(), newvarp, true));
		AstNode* rhsSelp = extendOrSel (pinp->fileline(), rhsp, pinexprp);
		assignp = new AstAssignW (pinp->fileline(), pinexprp, rhsSelp);
	    } else {
		// V3 width should have range/extended to make the widths correct
		assignp = new AstAssignW (pinp->fileline(),
					  new AstVarRef(pinp->fileline(), newvarp, true),
					  pinexprp);
		pinp->exprp(new AstVarRef (pinexprp->fileline(), newvarp, false));
	    }
	    if (assignp) cellp->addNextHere(assignp);
	    //if (debug()) { pinp->dumpTree(cout,"-  out:"); }
	    //if (debug()) { assignp->dumpTree(cout,"- aout:"); }
	}
	return assignp;
    }
};

//######################################################################
// Inst class functions

AstAssignW* V3Inst::pinReconnectSimple(AstPin* pinp, AstCell* cellp, AstNodeModule* modp,
				       bool forTristate, bool alwaysCvt) {
    return InstStatic::pinReconnectSimple(pinp, cellp, modp, forTristate, alwaysCvt);
}

//######################################################################
// Inst class visitor

void V3Inst::instAll(AstNetlist* nodep) {
    UINFO(2,__FUNCTION__<<": "<<endl);
    InstVisitor visitor (nodep);
    V3Global::dumpCheckGlobalTree("inst.tree", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 3);
}

void V3Inst::dearrayAll(AstNetlist* nodep) {
    UINFO(2,__FUNCTION__<<": "<<endl);
    InstDeVisitor visitor (nodep);
    V3Global::dumpCheckGlobalTree("dearray.tree", 0, v3Global.opt.dumpTreeLevel(__FILE__) >= 6);
}
