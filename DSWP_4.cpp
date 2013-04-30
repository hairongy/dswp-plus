//4th step: code splitting

#include "DSWP.h"

using namespace llvm;
using namespace std;

inline void replaceUses(PHINode *phi, map<Value*, Value*> repMap) {
	for (unsigned int j = 0, je = phi->getNumIncomingValues(); j < je; ++j) {
		Value *val = phi->getIncomingValue(j);
		if (Value *newArg = repMap[val]) {
			phi->setIncomingValue(j, newArg);
		}
	}
}

inline void replaceUses(User *user, map<Value*, Value*> repMap) {
	for (unsigned int j = 0, je = user->getNumOperands(); j < je; ++j) {
		Value *op = user->getOperand(j);
		if (Value *newArg = repMap[op]) {
			user->setOperand(j, newArg);
		}
	}
}


void DSWP::preLoopSplit(Loop *L) {
	// Makes the loop-replacement block that calls the worker threads.
	allFunc.clear();


	/*
	 * Insert a new block to replace the old loop
	 */
	replaceBlock = BasicBlock::Create(*context, "loop-replace", func);
	BranchInst *brInst = BranchInst::Create(exit, replaceBlock);
	replaceBlock->moveBefore(exit);

	// sanity check: the exit branch isn't in the loop
	// you know, in case we don't trust Loop::getExitBlock or something
	// NOTE: kind of pointless
	if (L->contains(exit)) {
		error("don't know why");
	}

	// point branches to the loop header to replaceBlock instead
	for (pred_iterator PI = pred_begin(header); PI != pred_end(header); ++PI) {
		BasicBlock *pred = *PI;
		if (L->contains(pred)) {
			continue;
		}
		TerminatorInst *termInst = pred->getTerminator();

		for (unsigned int i = 0; i < termInst->getNumOperands(); i++) {
			BasicBlock *bb = dyn_cast<BasicBlock>(termInst->getOperand(i));
			if (bb == header) {
				termInst->setOperand(i, replaceBlock);
			}
		}
	}

	// sanity check: nothing in the loop branches to the new replacement block
	// you know, in case we don't trust Loop::contains or something
	// NOTE: kind of pointless
	for (Loop::block_iterator li = L->block_begin(), le = L->block_end();
			li != le; li++) {
		BasicBlock *BB = *li;
		if (BB == replaceBlock) {
			error("the block should not appear here!");
		}
	}

	/*
	 * add functions for the worker threads
	 */

	Type *void_ty = Type::getVoidTy(*context),
	     *int32_ty = Type::getInt32Ty(*context),
	     *int64_ty = Type::getInt64Ty(*context),
	     *int8_ptr_t = Type::getInt8PtrTy(*context);

	// types for the functions: void* -> void*, b/c that's what pthreads wants
	vector<Type *> funArgTy;
	funArgTy.push_back(int8_ptr_t);
	FunctionType *fType = FunctionType::get(int8_ptr_t, funArgTy, false);

	// add the actual functions for each thread
	for (int i = 0; i < MAX_THREAD; i++) {
		Constant *c = module->getOrInsertFunction(
				itoa(loopCounter) + "_subloop_" + itoa(i), fType);
		if (c == NULL) {  // NOTE: don't think this is possible...?
			error("no function!");
		}
		Function *func = cast<Function>(c);
		func->setCallingConv(CallingConv::C);
		allFunc.push_back(func);
		generated.insert(func);
	}


	/*
	 * construct the argument struct
	 */

	// create a struct type to store the values of liveout vars in
	vector<Type *> liveoutTypes;
	for (unsigned int i = 0; i < liveout.size(); i++) {
		liveoutTypes.push_back(liveout[i]->getType());
	}
	StructType *outStructTy = StructType::create(
		*context, liveoutTypes, "outstruct_" + itoa(loopCounter) + "_ty");

	// create a struct type for the arguments to worker functions
	vector<Type *> argTypes;
	for (unsigned int i = 0; i < livein.size(); i++) {
		argTypes.push_back(livein[i]->getType());
	}
	argTypes.push_back(outStructTy);
	argStructTy = StructType::create(
		*context, argTypes, "argstruct_" + itoa(loopCounter) + "_ty");

	// allocate the argument struct
	AllocaInst *argStruct = new AllocaInst(argStructTy, "argstruct", brInst);

	// store the livein arguments
	for (unsigned int i = 0; i < livein.size(); i++) {
		// get the element pointer where we're storing this argument
		vector<Value *> gep_args;
		gep_args.push_back(ConstantInt::get(int64_ty, 0));
		gep_args.push_back(ConstantInt::get(int32_ty, i));
		GetElementPtrInst *ele_addr = GetElementPtrInst::CreateInBounds(
			argStruct, gep_args, livein[i]->getName() + "_argptr", brInst);

		// actually store it
		StoreInst *storeVal = new StoreInst(livein[i], ele_addr, brInst);
	}
	// NOTE: the output argument struct is left uninitialized

	/*
	 * initialize the communication queues
	 */
	Function *init = module->getFunction("sync_init");
	CallInst *callInit = CallInst::Create(init, "", brInst);


	/*
	 * call the worker functions
	 */
	Function *delegate = module->getFunction("sync_delegate");
	CastInst *argStruct_voidPtr = CastInst::CreatePointerCast(
		argStruct, int8_ptr_t,
		"argstruct_" + itoa(loopCounter) + "_cast", brInst);

	for (int i = 0; i < MAX_THREAD; i++) {
		vector<Value*> args;
		args.push_back(ConstantInt::get(int32_ty, i)); // the thread id
		args.push_back(allFunc[i]); // the function pointer
		args.push_back(argStruct_voidPtr); // the argument struct
		CallInst * callfunc = CallInst::Create(delegate, args, "", brInst);
	}


	/*
	 * join them back
	 */
	Function *join = module->getFunction("sync_join");
	CallInst *callJoin = CallInst::Create(join, "", brInst);

	/*
	 * load the liveout variables from the out struct
	 */
	if (!liveout.empty()) {
		// get the pointer to the output structure
		vector<Value *> gep_args;
		gep_args.push_back(ConstantInt::get(int64_ty, 0));
		gep_args.push_back(ConstantInt::get(int32_ty, livein.size()));
		GetElementPtrInst *out_addr = GetElementPtrInst::CreateInBounds(
			argStruct, gep_args, "load_outs", brInst);

		map<Value*, Value*> replacement_map;

		for (unsigned int i = 0; i < liveout.size(); i++) {
			// get the pointer to this out value
			gep_args.clear();
			gep_args.push_back(ConstantInt::get(int64_ty, 0));
			gep_args.push_back(ConstantInt::get(int32_ty, i));
			GetElementPtrInst *ele_addr = GetElementPtrInst::CreateInBounds(
				out_addr, gep_args, liveout[i]->getName() + "_ptr", brInst);

			// load the out value
			LoadInst *outVal = new LoadInst(
				ele_addr, liveout[i]->getName() + "_load", brInst);

			replacement_map[liveout[i]] = outVal;
		}

		// replace any uses *outside of the loop* with this load instruction
		for (Function::iterator bi = func->begin(), be = func->end();
				bi != be; ++bi) {
			BasicBlock &bb = *bi;
			if (!L->contains(&bb)) {
				for (BasicBlock::iterator ii = bb.begin(), ie = bb.end();
						ii != ie; ++ii) {
					replaceUses(&(*ii), replacement_map);
				}
			}
		}
	}
}


void DSWP::loopSplit(Loop *L) {
	//check for each partition, find relevant blocks, set could auto deduplicate

	for (int i = 0; i < MAX_THREAD; i++) {
		cout << "// Creating function for thread " + itoa(i) << endl;

		// create function body for each thread
		Function *curFunc = allFunc[i];


		/*
		 * figure out which blocks we use or are dependent on in this thread
		 */
		set<BasicBlock *> relbb;
		//relbb.insert(header);

		for (vector<int>::iterator ii = part[i].begin(), ie = part[i].end();
				ii != ie; ++ii) {
			int scc = *ii;
			for (vector<Instruction *>::iterator iii = InstInSCC[scc].begin(),
												 iie = InstInSCC[scc].end();
					iii != iie; ++iii) {
				Instruction *inst = *iii;
				relbb.insert(inst->getParent());

				// add blocks which the instruction is dependent on
				const vector<Edge> &edges = rev[inst];
				for (vector<Edge>::const_iterator ei = edges.begin(),
												  ee = edges.end();
						ei != ee; ei++) {
					Instruction *dep = ei->v;
					relbb.insert(dep->getParent());
				}
			}
		}

		if (relbb.size() == 0) {
			error("no related blocks?");
		}

		/*
		 * Create the new blocks for the new function, including entry and exit
		 */
		map<BasicBlock *, BasicBlock *> BBMap; // map old blocks to new block

		BasicBlock *newEntry =
				BasicBlock::Create(*context, "new-entry", curFunc);
		BasicBlock *newExit = BasicBlock::Create(*context, "new-exit", curFunc);

		// make copies of the basic blocks
		for (set<BasicBlock *>::iterator bi = relbb.begin(), be = relbb.end();
				bi != be; ++bi) {
			BasicBlock *BB = *bi;
			BBMap[BB] = BasicBlock::Create(*context,
					BB->getName().str() + "_" + itoa(i), curFunc, newExit);
		}
		BBMap[predecessor] = newEntry;
		BBMap[exit] = newExit;

		if (BBMap[header] == NULL) {
			error("this must be a error early in dependency analysis stage");
		}

		// branch from the entry block to the new header
		BranchInst *newToHeader = BranchInst::Create(BBMap[header], newEntry);

		// return null
		ReturnInst *newRet = ReturnInst::Create(*context,
				Constant::getNullValue(Type::getInt8PtrTy(*context)), newExit);

		/*
		 * copy over the instructions in each block
		 */
		for (set<BasicBlock *>::iterator bi = relbb.begin(), be = relbb.end();
				bi != be; bi++) {
			BasicBlock *BB = *bi;
			BasicBlock *NBB = BBMap[BB];

			for (BasicBlock::iterator ii = BB->begin(), ie = BB->end();
					ii != ie; ii++) {
				Instruction *inst = ii;

				if (assigned[sccId[inst]] != i && !isa<TerminatorInst>(inst)) {
					// TODO I NEED TO CHECK THIS BACK
					// NOTE: that's is from the original code. what'd he mean?
					continue;
				}

				Instruction *newInst = inst->clone();
				if (inst->hasName()) {
					newInst->setName(inst->getName() + "_" + itoa(i));
				}

				// re-point branches and such to new blocks
				if (isa<TerminatorInst>(newInst)) {
					// re-point any successor blocks
					for (unsigned int j = 0, je = newInst->getNumOperands();
							j < je; j++) {
						Value *op = newInst->getOperand(j);

						if (BasicBlock *oldBB = dyn_cast<BasicBlock>(op)) {
							BasicBlock *newBB = BBMap[oldBB];

							if (oldBB != exit && !L->contains(oldBB)) {
								// branching to a block outside the loop that's
								// not the exit. this should be impossible...
								error("crazy branch :(");
								continue;
							}

							// if we branched to a block not in this thread,
							// go to the next post-dominator
							// NOTE: right?
							while (newBB == NULL) {
								oldBB = postidom[oldBB];
								newBB = BBMap[oldBB];
								if (oldBB == NULL) {
									error("postdominator info seems broken :(");
									break;
								}
							}

							// replace the target block
							newInst->setOperand(j, newBB);

							// TODO check if there are two branch, one branch
							// is not in the partition, then what the branch
						}
					}
				}
				else if (PHINode *phi = dyn_cast<PHINode>(newInst)) {
					// re-point block predecessors of phi nodes
					// values will be re-pointed later on
					for (unsigned int j = 0, je = phi->getNumIncomingValues();
							j < je; j++) {
						BasicBlock *oldBB = phi->getIncomingBlock(j);
						BasicBlock *newBB = BBMap[oldBB];

						// if we're branching from a block not in this thread,
						// go to the previous dominator of that block
						// NOTE: is this the right thing to do?
						while (newBB == NULL) {
							oldBB = idom[oldBB];
							newBB = BBMap[oldBB];
							if (oldBB == NULL) {
								error("dominator info seems broken :(");
								break;
							}
						}

						// replace the previous target block
						phi->setIncomingBlock(j, newBB);
					}
				}

				instMap[i][inst] = newInst;
				newInstAssigned[newInst] = i;
				newToOld[newInst] = inst;

				//newInst->dump();
				NBB->getInstList().push_back(newInst);
			}
		}

		/*
		 * Load the arguments, replacing livein variables
		 */
		Function::ArgumentListType &arglist = curFunc->getArgumentList();
		if (arglist.size() != 1) {
			error("argument size error!");
		}
		Argument *args = arglist.begin(); //the function only have one argmument

		Function *showPlace = module->getFunction("showPlace");
		CallInst *inHeader = CallInst::Create(showPlace);
		inHeader->insertBefore(newToHeader);

		BitCastInst *castArgs = new BitCastInst(
				args, PointerType::get(argStructTy, 0), "args");
		castArgs->insertBefore(newToHeader);

		for (unsigned int j = 0, je = livein.size(); j < je; j++) {
			cout << "Handling argument: " << livein[j]->getName().str() << endl;

			// get pointer to the jth argument
			vector<Value *> gep_args;
			gep_args.push_back(ConstantInt::get(Type::getInt64Ty(*context), 0));
			gep_args.push_back(ConstantInt::get(Type::getInt32Ty(*context), j));
			GetElementPtrInst* ele_addr = GetElementPtrInst::Create(
				castArgs, gep_args, livein[j]->getName() + "_arg", newToHeader);

			// load it
			LoadInst *ele_val = new LoadInst(ele_addr);
			ele_val->setAlignment(8); // TODO do we want this?
			ele_val->setName(livein[j]->getName().str() + "_val");
			ele_val->insertBefore(newToHeader);

			/*
			// debug: show the value
			vector<Value *> showArg;
			showArg.push_back(ele_val);
			Function *show = module->getFunction("showValue");
			CallInst *callShow = CallInst::Create(show, showArg);
			callShow->insertBefore(newToHeader);
			*/

			if (ele_val->getType() != livein[j]->getType()) {
				error("broken type for " + livein[j]->getName().str());
			}

			instMap[i][livein[j]] = ele_val;
		}

		/*
		 * Replace the use of instruction def in the function.
		 * reg dep should be finished in insert syn
		 */
		for (inst_iterator ii = inst_begin(curFunc), ie = inst_end(curFunc);
				ii != ie; ++ii) {
			Instruction *inst = &(*ii);
			replaceUses(inst, instMap[i]);
		}

		/*
		 * Store any liveout variables defined in this thread.
		 */
		Type *int32_ty = Type::getInt32Ty(*context),
		     *int64_ty = Type::getInt64Ty(*context);

		// get the pointer to the output structure
		vector<Value *> gep_args;
		gep_args.push_back(ConstantInt::get(int64_ty, 0));
		gep_args.push_back(ConstantInt::get(int32_ty, livein.size()));
		GetElementPtrInst *out_addr = GetElementPtrInst::CreateInBounds(
			castArgs, gep_args, "load_outs", newRet);

		for (unsigned int j = 0; j < liveout.size(); j++) {
			if (getInstAssigned(liveout[j]) == i) {
				// get the pointer where we want to save this
				gep_args.clear();
				gep_args.push_back(ConstantInt::get(int64_ty, 0));
				gep_args.push_back(ConstantInt::get(int32_ty, j));
				GetElementPtrInst *ele_addr = GetElementPtrInst::CreateInBounds(
					out_addr, gep_args, liveout[j]->getName() + "_outptr",
					newRet);

				// save it
				StoreInst *store = new StoreInst(
					instMap[i][liveout[j]], ele_addr, newRet);
			}
		}
	}
}

void DSWP::getDominators(Loop *L) {
	DominatorTree &dom_tree = getAnalysis<DominatorTree>();
	PostDominatorTree &postdom_tree = getAnalysis<PostDominatorTree>();

	for (Function::iterator bi = func->begin(); bi != func->end(); bi++) {
		BasicBlock *BB = bi;

		DomTreeNode *idom_node = dom_tree.getNode(BB)->getIDom();
		idom[BB] = idom_node == NULL ? NULL : idom_node->getBlock();

		DomTreeNode *postidom_node = postdom_tree.getNode(BB)->getIDom();
		postidom[BB] = postidom_node == NULL ? NULL : postidom_node->getBlock();
	}
}


void DSWP::getLiveinfo(Loop *L) {
	defin.clear();
	livein.clear();
	liveout.clear();

	// Figure out which variables are live.
	// Don't want to use standard liveness analysis....

	// Find variables defined in the loop, and those that are used outside
	// the loop.
	for (Loop::block_iterator bi = L->getBlocks().begin(),
							  be = L->getBlocks().end();
			bi != be; ++bi) {
		BasicBlock *BB = *bi;
		for (BasicBlock::iterator ii = BB->begin(), ie = BB->end();
				ii != ie; ++ii) {
			Instruction *inst = &(*ii);
			if (util.hasNewDef(inst)) {
				defin.push_back(inst);
			}
			bool already_liveouted = false;
			for (Instruction::use_iterator ui = inst->use_begin(),
										   ue = inst->use_end();
					ui != ue; ++ui) {
				User *use = *ui;
				if (Instruction *use_i = dyn_cast<Instruction>(use)) {
					if (!already_liveouted && !L->contains(use_i)) {
						liveout.push_back(inst);
						already_liveouted = true;
					}
				} else {
					error("used by something that's not an instruction???");
				}
			}
		}
	}

	// Anything used in the loop that's not defined in it is a livein variable.
	for (Loop::block_iterator bi = L->getBlocks().begin(),
							  be = L->getBlocks().end();
			bi != be; ++bi) {
		BasicBlock *BB = *bi;
		for (BasicBlock::iterator ii = BB->begin(), ie = BB->end();
				ii != ie; ++ii) {
			Instruction *inst = &(*ii);

			for (Instruction::op_iterator oi = inst->op_begin(),
										  oe = inst->op_end();
					oi != oe; ++oi) {
				Value *op = *oi;
				if ((isa<Instruction>(op) || isa<Argument>(op)) &&
						find(defin.begin(), defin.end(), op) == defin.end() &&
						find(livein.begin(), livein.end(), op) == livein.end()) {
					// the operand is a variable that isn't defined inside the
					// loop and hasn't already been counted as livein
					livein.push_back(op);
				}
			}
		}
	}
}
