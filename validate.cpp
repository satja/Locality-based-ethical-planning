#include <iostream>
#include <unordered_map>
#include <vector>
#include <string>
#include <cctype>
#include <cassert>

/*
Validator overview (based on paper.tex and input-output-explanation.txt):
- Input is read from stdin.
- Line 1: comma-separated local propositions.
- Line 2: comma-separated global propositions.
- Line 3: comma-separated initial truth values for locals (TRUE/FALSE).
- Line 4: comma-separated initial truth values for globals (TRUE/FALSE).
- Remaining lines:
  - "- action proposition : formula" defines gamma^- preconditions.
  - "+ action proposition : formula" defines gamma^+ preconditions.
  - "l : formula" defines a local value/formula.
  - "g : formula" defines a global value/formula.
  - Otherwise, the line is treated as the action sequence (space-separated).

Execution model:
- For each action a_t, gamma^- and gamma^+ are evaluated in the current state.
- If gamma^-(a_t, p) is true, p becomes false.
- If gamma^+(a_t, p) is true, p becomes true.
- Values/formulas are checked at the initial state and after each action.
- Only violations are printed.

Note: input may contain CRLF line endings; we strip trailing '\r'.
*/

std::unordered_map<std::string, int> lv_key;//local have different key than global
std::unordered_map<std::string, int> gv_key;
std::unordered_map<int,int> lKey_index;
std::unordered_map<int,int> gKey_index;
std::unordered_map<std::string,int> operator_key = {{"G",0},{"FG",1},{"F",2},{"X",3},{"U",4},{"NOT",5},{"AND",6},{"OR",7}};
std::unordered_map<int,std::string> key_operator = {{0,"G"},{1,"FG"},{2,"F"},{3,"X"},{4,"U"},{5,"NOT"},{6,"AND"},{7,"OR"}};
int *lv_values;
int *gv_values;
int lvSize = 0, gvSize = 0;
std::vector<std::string> actions;

struct NamedFormula {
    std::string text;
    class formula* node;
};

static int getValueForKey(int key) {
    auto lit = lKey_index.find(key);
    if (lit != lKey_index.end()) {
        assert(lv_values != nullptr);
        assert(lit->second >= 0 && lit->second < lvSize);
        return lv_values[lit->second];
    }

    auto git = gKey_index.find(key);
    assert(git != gKey_index.end());
    assert(gv_values != nullptr);
    assert(git->second >= 0 && git->second < gvSize);
    return gv_values[git->second];
}

static std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        start++;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        end--;
    }
    return s.substr(start, end - start);
}

static std::string strip_outer_parens(const std::string& s) {
    std::string out = trim(s);
    if (out.size() < 2 || out.front() != '(' || out.back() != ')') {
        return out;
    }
    int depth = 0;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] == '(') depth++;
        else if (out[i] == ')') depth--;
        if (depth == 0 && i != out.size() - 1) {
            return out;  // outer parens do not wrap the full string
        }
    }
    return trim(out.substr(1, out.size() - 2));
}

static bool split_top_level_ops(const std::string& expr,
                                std::vector<std::string>& parts_out,
                                std::vector<int>& ops_out) {
    std::string current;
    int depth = 0;
    auto match_word = [&](size_t i, const std::string& word) {
        if (i + word.size() > expr.size()) return false;
        if (expr.compare(i, word.size(), word) != 0) return false;
        const bool left_ok = (i == 0) || std::isspace(static_cast<unsigned char>(expr[i - 1]));
        const bool right_ok =
            (i + word.size() == expr.size()) ||
            std::isspace(static_cast<unsigned char>(expr[i + word.size()]));
        return left_ok && right_ok;
    };

    for (size_t i = 0; i < expr.size(); i++) {
        const char ch = expr[i];
        if (ch == '(') depth++;
        if (ch == ')') depth--;

        if (depth == 0) {
            if (match_word(i, "AND")) {
                parts_out.push_back(trim(current));
                current.clear();
                ops_out.push_back(operator_key["AND"]);
                i += 2;
                continue;
            }
            if (match_word(i, "OR")) {
                parts_out.push_back(trim(current));
                current.clear();
                ops_out.push_back(operator_key["OR"]);
                i += 1;
                continue;
            }
            if (match_word(i, "U")) {
                parts_out.push_back(trim(current));
                current.clear();
                ops_out.push_back(operator_key["U"]);
                continue;
            }
        }
        current.push_back(ch);
    }

    if (!current.empty()) {
        parts_out.push_back(trim(current));
    }

    return !ops_out.empty();
}

class formula{

   std::vector<formula*> parts;
   std::vector<int> operators;
   std::vector<int> variables;
   std::vector<int> xPrepared;
   std::vector<int> xTrue;
   int xInd = 0;
   std::vector<int> uRes;
   std::vector<int> fTrue;
   int fInd = 0, uInd = 0;
   std::vector<int> gFalse;
   int gInd = 0;
   int lastAction = 0;
   int fixed, value;
   int TrueConstant = 0, FalseConstant=0;

    public: 

    formula(){
        fixed = value = 0;
    }

    void setLastAction(){
       lastAction = 1;
    }

    formula(std::string f){
        fixed = value = 0;
        std::string op="";
        int ind  = 0;
        // Parse temporal wrappers and parenthesized expressions first.
        if(f[0] == '('){//case ( FG (v1 op1 v2 op2 .... vk )) or ( FG ((v1 op 1 v2 op ... vk) op () ... op ())
           const bool maybeUnaryPrefix = f.size() >= 5 && f[1] != '(';
           if(maybeUnaryPrefix && f[2] == 'F'){
                 op+="F";
                 if(f[3] == 'G'){
                    if(f[4] == ' '){
                      op+="G";
                      ind = 4;
                    }
                 }
                 else if(f[3] == ' ') ind = 3;
           }
           else if(maybeUnaryPrefix && f[2] == 'G'){
                  op+="G";
                  if(f[3] == 'F'){
                    if(f[4] == ' '){
                      op+="F";
                      ind = 4;
                    }
                  }
                  else if(f[3] == ' ') ind = 3;
           }
           else if(maybeUnaryPrefix && f[2] == 'X'){
                    op+="X";
                    ind = 3;
           }
           else if(maybeUnaryPrefix && f[2] == 'N' && f[3] == 'O' && f[4] == 'T'){
                  op+="NOT";
                  ind = 5;
           }

           if(ind == 3 || ind == 4 || ind == 5){
            // Unary operator case: parse the wrapped subformula recursively.
            //std::cout<<"OP1: "<<op<<std::endl;
            if(op!=""){
          //    std::cout<<"op: "<<op<<std::endl;
           operators.push_back(operator_key[op]);
            }
           std::string subf = "";
           for(int i=ind+2; i<f.size()-2;i++)
                subf+=f[i];
          //  std::cout<<"Subformula: "<<subf<<std::endl;
            formula *fs = new formula(subf);
            parts.push_back(fs);
           }
           else{//case (v1 op1 v2 op2 .... vk ) op () .... ()
              // Try a top-level split on AND/OR/U with balanced parentheses.
              std::string stripped = strip_outer_parens(f);
              std::vector<std::string> top_parts;
              std::vector<int> top_ops;
              if (split_top_level_ops(stripped, top_parts, top_ops)) {
                for (const std::string& part : top_parts) {
                    if (!part.empty()) {
                        formula* fs = new formula(part);
                        parts.push_back(fs);
                    }
                }
                operators = top_ops;
                return;
              }

              // If this is just extra wrapping parentheses around one
              // subformula (e.g., "(NOT (p))"), recurse on the stripped body.
              if (stripped != f) {
                formula* fs = new formula(stripped);
                parts.push_back(fs);
                return;
              }

              if(f[0] == '(' && f[1] == '('){ //case ((v1 op1 v2 op20 op () ... ()))
                        std::string tmp = "";
                        for(int i=1;i<f.size()-1;i++)
                             tmp+=f[i];
                      f = tmp;
              }

             std::string subf = "";

             // Walk the parenthesized chain and split into subformulas/operators.
             for(int i=1;i<f.size()-1;i++){
                if(f[i]!=')' && f[i]!='(')
                subf+=f[i];
                else if(f[i] == '(') continue;
                else{
                    formula *fs = new formula(subf);
                    parts.push_back(fs);
                    subf = "";
                    if(i == f.size()-1) break;
                    int j = i+2;
                    while(f[j]!=' '){
                        subf+=f[j];
                        j++;
                    }
                    //std::cout<<"OP2: "<<subf<<std::endl;
                    if(subf!=""){
                  //    std::cout<<"op1: "<<subf<<std::endl;
                    operators.push_back(operator_key[subf]);
                    }
                    subf = "";
                }
             }

           }

        }
        else{//v1 op1 v2 op2 ... OR NOT (v1) opt1 NOT (v2) opt ...

                // Handle constants and single literals early.
                if(f == "TRUE"){
                     TrueConstant = 1;
                     return;
                }
                else if(f == "FALSE"){
                     FalseConstant = 1;
                     return;
                }
           
               if(lv_key.find(f) != lv_key.end()){//only vi
                       variables.push_back(lv_key[f]);
                       return;
               }
               else if(gv_key.find(f) != gv_key.end()){//onlly vi
                      variables.push_back(gv_key[f]);
                       return;
               }

               // Handle top-level binary chains (including U) even without
               // outer parentheses, e.g., "NOT (p) U q".
               std::vector<std::string> top_parts;
               std::vector<int> top_ops;
               if (split_top_level_ops(f, top_parts, top_ops)) {
                    for (const std::string& part : top_parts) {
                        if (!part.empty()) {
                            formula *fs = new formula(part);
                            parts.push_back(fs);
                        }
                    }
                    operators = top_ops;
                    return;
               }

               std::string subf = ""; //more complex case
               int i=0;
               if(f[0] == ' ') i=1;
               // Parse NOT/AND/OR chains by alternating operators and parts.
               for(;i<f.size();i++){
                        while(f[i]!=' ' && i<f.size()){
                             subf+=f[i];
                             i++;
                        }
                         //std::cout<<"Complex: "<<f<<std::endl;
                        // std::cout<<"Complex subf: "<<subf<<std::endl;
                        if(subf == "NOT" || subf == "AND" || subf == "OR"){
                          //std::cout<<"OP3: "<<subf<<std::endl;
                          if(subf!=""){
                          //  std::cout<<"op3: "<<subf<<std::endl;
                           operators.push_back(operator_key[subf]);
                          }
                           subf = ""; i++;
                           int c = 0;
                           if(f[i] == '('){ i++; c++;}
                           while(f[i]!=')' && i<f.size()){
                            if(f[i] == '(') c++;
                             subf+=f[i];
                             i++;
                        }
                        if(c == 2) subf+=")";
                        
                       // std::cout<<"Subformula for recursion: "<<subf<<std::endl;
                           formula *fs = new formula(subf);
                            parts.push_back(fs);
                            subf = "";
                        }
                        else{            

                        if(lv_key.find(subf) != lv_key.end()){
                            // variables.push_back(lv_key[subf]);
                            formula *fs = new formula(subf);
                            parts.push_back(fs);
                            subf = "";
                        }
                        else if(gv_key.find(subf) != gv_key.end()){
                             //variables.push_back(gv_key[subf]);
                             formula *fs = new formula(subf);
                            parts.push_back(fs);
                            subf = "";
                          } 
                        }
                        subf = "";  
                        
                        if( i == f.size()-1) break;

                       while(f[i]!=' ' && f[i] !=')' && i<f.size()){
                             subf+=f[i];
                             i++;
                        }
                        //std::cout<<"OP4: "<<subf<<std::endl;
                        if(subf!=""){
                     //     std::cout<<"op4: "<<subf<<std::endl;
                        operators.push_back(operator_key[subf]);
                        }

               }
        }

            //starts with (
               //contains F, G, FG or GF
               //contains up to k variables and k-1 operators -> potentially following operators and other groups
            //starts with a variable -> following operators and variables
     }
    
     int evaluate(){//watch on literal level negation

      if(parts.empty()){ //assumption, at least 2 variables and one operator

        if(TrueConstant) return 1;
        if(FalseConstant) return 0;

        assert(!variables.empty());

        if(variables.size()==1){
          // Single literal: read the current value directly.
          int varA = variables[0];
          int val1 = getValueForKey(varA);
          return val1; 
        }

         int truth = 0;
         assert(variables.size() >= 2);
         assert(!operators.empty());
         // Binary literal case: evaluate using the first operator.
         int varA = variables[0];
         int varB = variables[1];

         int val1 = getValueForKey(varA);
         int val2 = getValueForKey(varB);

         std::string op = key_operator[operators[0]];
         if(op == "AND"){

             truth = val1 && val2;
             return truth;
         }
         else if(op == "OR"){
             truth = val1 || val2;
             return truth;
         }
         else if(op == "U"){
             if(uRes.size() > uInd){
                 if(uRes[uInd] == 1) return 1;
                 if(uRes[uInd] == -1) return 0;
             }
             if(!val1 && !val2){ // left false, right false -> until false
                 uRes.push_back(-1);
                 uInd++;
                 return 0;
             }
             if(val1 && !val2) return 1; // left true, right still false
             if(val2){ // right true, left not reported false before
                 uRes.push_back(1);
                 uInd++;
                 return 1;
             }
         }
         return truth;
      }
      else{//case NOT (X, F, G, FG) (v1) op1 v2 op2 NOT (v2) op3 v3 op4 ...
             if(operators.empty()){
                // Degenerate wrapped form, e.g., "(NOT (p))" after stripping.
                assert(parts.size() == 1);
                return parts[0]->evaluate();
             }
             int truth = 0, partsInd = 0;

           // When parsing as top-level binary parts (e.g., a AND b),
           // seed `truth` with the left operand before folding operators.
           if(!operators.empty()){
              std::string firstOp = key_operator[operators[0]];
              if(firstOp == "AND" || firstOp == "OR" || firstOp == "U"){
                 assert(partsInd < parts.size());
                 truth = parts[partsInd]->evaluate();
                 partsInd++;
              }
           }
        
            
           // Evaluate the operator stream left-to-right, maintaining indices.
           for(int i=0;i<operators.size();i++) {
             std::string op = key_operator[operators[i]];
             

              if(op == "NOT"){//oblik NOT (subf) op2 ()....
                   // Negation applies to the next part.
                   assert(partsInd < parts.size());
                   truth = parts[partsInd]->evaluate();
                   truth = 1 - truth;
                   partsInd++;
              }
              else if(op == "X"){//provjeriti u sljedecem stanju
                  // X is deferred by one step using xPrepared/xTrue memory.
                  if(xPrepared.size()<=xInd){
                     xPrepared.push_back(1); xInd++;
                     truth = 1; //evaluiraj ostatak formule
                  }
                  else{
                     if(xTrue.size()<=xInd){
                            assert(partsInd < parts.size());
                            truth = parts[partsInd]->evaluate();
                            xTrue.push_back(truth);
                            xInd++;
                            partsInd++;
                     }
                     else{
                            truth = xTrue[xInd];
                     }
                  }   
              }
              else if(op == "F"){
                   // F becomes permanently true once observed true.
                   if(fTrue.size()<=fInd){
                   assert(partsInd < parts.size());
                   truth = parts[partsInd]->evaluate();
                   partsInd++;
                   if(truth == 1){
                     fTrue.push_back(1); fInd++;
                   }
                 }
                 else truth = 1; //istinit u nekom prethodnom trenutku
              }
              else if(op == "G"){
                    // G becomes permanently false once observed false.
                    if(gFalse.size()<=gInd){
                          assert(partsInd < parts.size());
                          truth = parts[partsInd]->evaluate();
                          partsInd++;
                          if(truth == 0){
                            gFalse.push_back(1); gInd++;
                          }
                    }
                    else truth = 0; //nekada laz
              }
              else if(op == "FG"){
                      // FG is evaluated only at the last action.
                      if(lastAction == 1){
                            assert(partsInd < parts.size());
                            truth = parts[partsInd]->evaluate();
                            partsInd++;
                      }
                      else truth = 1; //dopusti evaluaciju ostatka formule
              }
              else if(op == "AND"){
                      // Handle AND with an optional following NOT.
                      if(i + 1 < operators.size()){
                        if(key_operator[operators[i+1]] == "NOT"){
                              assert(partsInd < parts.size());
                              truth  = truth && (1-parts[partsInd]->evaluate());
                              partsInd++; i++;
                        }
                        else{
                               assert(partsInd < parts.size());
                               truth = truth && parts[partsInd]->evaluate();
                               partsInd++;
                        }
                      }
                      else{//nema operatora iza AND
                              assert(partsInd < parts.size());
                              truth = truth && parts[partsInd]->evaluate();
                              partsInd++;     
                      }
              }
              else if(op == "OR"){
                     // Handle OR with an optional following NOT.
                     if(i + 1 < operators.size()){
                        if(key_operator[operators[i+1]] == "NOT"){
                              assert(partsInd < parts.size());
                              truth  = truth || (1-parts[partsInd]->evaluate());
                              partsInd++; i++;
                        }
                        else{
                               assert(partsInd < parts.size());
                               truth = truth || parts[partsInd]->evaluate();
                               partsInd++;
                        }
                      }
                      else{//nema operatora iza OR
                              assert(partsInd < parts.size());
                              truth = truth || parts[partsInd]->evaluate();
                              partsInd++;     
                      }
              }
              else if(op == "U"){
                      // The left side of U is in `truth`; now evaluate right side.
                      int t1 = 0;
                      if(i + 1 < operators.size() && key_operator[operators[i+1]] == "NOT"){
                          assert(partsInd < parts.size());
                          t1 = (1 - parts[partsInd]->evaluate());
                          partsInd++;
                          i++;
                      } else {
                          assert(partsInd < parts.size());
                          t1 = parts[partsInd]->evaluate();
                          partsInd++;
                      }

                      if(uRes.size() > uInd){
                          if(uRes[uInd] == 1) {
                              truth = 1;
                          } else if(uRes[uInd] == -1) {
                              truth = 0;
                          }
                      } else {
                          if(!truth && !t1){
                              uRes.push_back(-1);
                              uInd++;
                              truth = 0;
                          }
                          if(t1){
                              uRes.push_back(1);
                              uInd++;
                              truth = 1;
                          }
                      }
              }
            }
                // Reset per-step indices after each evaluation call.
                xInd = fInd = gInd = uInd = 0;
                return truth;  
      } 
      return 0;
     }

     void print(int space){

       if(TrueConstant) std::cout<<"TRUE"<<std::endl;
       else if(FalseConstant) std::cout<<"FALSE"<<std::endl;

          if(operators.size()>0){

             std::string op = key_operator[operators[0]];
             int partInd = 0;

         if(op == "NOT" || op == "G" || op == "F" || op == "FG" || op == "X"){ 
              for(int i=0;i<=space;i++) std::cout<<" ";
              std::cout<<op<<std::endl;
              parts[0]->print(space+1);
              partInd = 1;
         }
         else{
             parts[0]->print(space+1);
             for(int i=0;i<=space;i++) std::cout<<" ";
             std::cout<<op<<std::endl;
             parts[1]->print(space+1);
             partInd = 2;
         }

            for(int i=1;i<operators.size();i++){
              std::string op = key_operator[operators[i]];
                  if(op == "NOT" || op == "G" || op == "F" || op == "FG" || op == "X"){ 
              for(int i=0;i<=space;i++) std::cout<<" ";
              std::cout<<op<<std::endl;
              parts[partInd++]->print(space+1);
                   }
                   else{
                        for(int i=0;i<=space;i++) std::cout<<" ";
                        std::cout<<op<<std::endl;
                        parts[partInd++]->print(space+1);
                   }
            }
          }
          else if(variables.size() == 1){
            for(int i=0;i<=space;i++) std::cout<<" ";
            std::string variable = "";
              for (std::unordered_map<std::string,int>::const_iterator it = lv_key.begin(); it != lv_key.end(); ++it) {
                     if (it->second == variables[0]) variable = it->first;
                 }
              
              if(variable == ""){
                 for (std::unordered_map<std::string,int>::const_iterator it = gv_key.begin(); it != gv_key.end(); ++it) {
                     if (it->second == variables[0]) variable = it->first;
                 }
              }
              for(int i=0;i<=space;i++) std::cout<<" ";
              std::cout<<variable<<" ";
       }

     }

};

std::vector<NamedFormula> local_formulas;
std::vector<NamedFormula> global_formulas;

std::unordered_map<std::string,std::unordered_map<std::string,formula*>> gammaMinus;
std::unordered_map<std::string,std::unordered_map<std::string,formula*>> gammaPlus;

/*
Parse the validator input format from stdin.
This function intentionally performs a single linear pass over lines and
interprets them by their position (first four lines) and leading character.
*/
void readProblem(std::istream &input){
         // Read propositions (local/global) and build key/index tables.
         int key = 0, lind = 0, gind = 0; 
         std::string s;
        int line = 1;
       while (std::getline(input,s))
        {
             // Normalize CRLF line endings to LF for robust parsing.
             if (!s.empty() && s.back() == '\r') {
                 s.pop_back();
             }
             // Skip empty lines rather than treating them as action sequences.
             if (s.empty()) {
                 continue;
             }
             if(line == 1){
              // Line 1: comma-separated local proposition names.
              std::string tmp="";
              for(int i=0;i<s.size();i++){
                  if(s[i]!=','){
                      tmp+=s[i];
                      if(i == s.size()-1){
                        lv_key[tmp] = key;
                        lKey_index[key] = lind;
                        key++; lind++; i++;
                         tmp = "";
                      }
                  }
                  else if(s[i] == ','){//add to structures
                       lv_key[tmp] = key;
                       lKey_index[key] = lind;
                       key++; lind++; i++;
                        tmp = "";
                  }
                 
                }
             }
             else if(line == 2){
              // Line 2: comma-separated global proposition names.
              std::string tmp="";
              for(int i=0;i<s.size();i++){
                  if(s[i]!=','){
                      tmp+=s[i];
                      if(i == s.size()-1){
                        gv_key[tmp] = key;
                        gKey_index[key] = gind;
                        key++; gind++; i++;
                        tmp = "";
                      }
                  }
                  else if(s[i] == ','){//add to structures
                       gv_key[tmp] = key;
                       gKey_index[key] = gind;
                       key++; gind++; i++; 
                       tmp = "";
                  }
                }
                
             }
             else if(line == 3){
               // Line 3: initial local truth values (allocates value arrays).
               lv_values = new int[lind];
               gv_values = new int[gind];
               lvSize = lind;
               gvSize = gind;
                   
                   std::string tmp=""; int ind = 0;
              for(int i=0;i<s.size();i++){
                  if(s[i]!=','){
                      tmp+=s[i];
                      if(i == s.size()-1){
                        if(tmp == "TRUE")
                          lv_values[ind] = 1;
                       else lv_values[ind] = 0;
                       ind++; i++;  tmp = "";
                      }
                  }
                  else if(s[i] == ','){//add to structures
                       if(tmp == "TRUE")
                          lv_values[ind] = 1;
                       else lv_values[ind] = 0;
                       ind++; i++;
                       tmp = "";
                  }
                }
                 
             }
             else if(line == 4){
                    // Line 4: initial global truth values.
                    std::string tmp=""; int ind = 0;
              for(int i=0;i<s.size();i++){
                  if(s[i]!=','){
                      tmp+=s[i];
                      if(i == s.size()-1){
                        if(tmp == "TRUE")
                          gv_values[ind] = 1;
                       else gv_values[ind] = 0;
                       ind++; i++; tmp = "";
                      }
                  }
                  else if(s[i] == ','){//add to structures
                       if(tmp == "TRUE")
                          gv_values[ind] = 1;
                       else gv_values[ind] = 0;
                       ind++; i++; tmp = "";
                  }
                 
                }
             }
             else{
               // Remaining lines: gamma entries, values, or the action sequence.
               if(s[0] == '-'){
                   // gamma^- line: "- action proposition : formula"
                   std::string act="", prop="", form="";
                   int part = 0;
                   for(int i=2;i<s.size();i++){
                          if(part == 0 && s[i]!=' ') act+=s[i];   
                          else if(part == 0 && s[i] == ' ') part++;
                          else if(part == 1 && s[i]!=' ') prop+=s[i];
                          else if(part == 1 && s[i] == ' '){ part++; i+=2;}
                          else if(part == 2){
                              form+=s[i];
                          }
                   }

                   if (gammaMinus.find(act) != gammaMinus.end())
                       {
                          formula *f = new formula(form);
                          gammaMinus[act][prop] = f;
                     } 
                  else{
                    std::unordered_map<std::string,formula*> nm;
                    formula *f = new formula(form);
                    nm[prop] = f;
                    gammaMinus[act] = nm;
                  }
               }
               else if(s[0] == '+'){
                   // gamma^+ line: "+ action proposition : formula"
                   std::string act="", prop="", form="";
                   int part = 0;
                   for(int i=2;i<s.size();i++){
                          if(part == 0 && s[i]!=' ') act+=s[i];   
                          else if(part == 0 && s[i] == ' ') part++;
                          else if(part == 1 && s[i]!=' ') prop+=s[i];
                          else if(part == 1 && s[i] == ' '){ part++; i+=2;}
                          else if(part == 2){
                              form+=s[i];
                          }
                   }

                   if (gammaPlus.find(act) != gammaPlus.end())
                       {
                          formula *f = new formula(form);
                          gammaPlus[act][prop] = f;
                     } 
                  else{
                    std::unordered_map<std::string,formula*> nm;
                    formula *f = new formula(form);
                    nm[prop] = f;
                    gammaPlus[act] = nm;
                  }
               }
               else if(s[0] == 'l' && s[2] == ':'){
                // Local value formula: "l : <formula>"
                std::string st = "";
                for(int i=4;i<s.size();i++)
                          st+=s[i];
                formula *f = new formula(st);
                 local_formulas.push_back({st, f});
               }
               else if(s[0] == 'g' && s[2] == ':'){
                // Global value formula: "g : <formula>"
                    std::string st = "";
                for(int i=4;i<s.size();i++)
                          st+=s[i];
                formula *f = new formula(st);
                 global_formulas.push_back({st, f});
               }
               else{//readActions
                    // Action sequence: space-separated action names.
                    std::string act="";
                      for(int i=0;i<s.size();i++){
                        if(s[i]!=' ') act+=s[i];
                        else{
                          actions.push_back(act);
                          act="";
                        }
                      }
                      actions.push_back(act);
             }
           }
             
             line++;
         }


         //read actions ?
         //read gamma (+/-)
         //read formulae (l/g)
         //read the sequence of actions
}

/*
Evaluate all formulas in the current global state. This advances the temporal
memory stored inside each formula node, so it should be called exactly once
per time step (including the initial state).
*/
static bool evaluateFormulas(std::vector<NamedFormula>& formulas,
                             const std::string& scope,
                             int step,
                             const std::string& action) {
    bool ok = true;
    for (NamedFormula& nf : formulas) {
        assert(nf.node != nullptr);
        const int val = nf.node->evaluate();
        if (val == 0) {
            ok = false;
            if (step == 0) {
                std::cout << scope << " formula violated at initial state: "
                          << nf.text << std::endl;
            } else {
                std::cout << scope << " formula violated after step " << step
                          << " (action " << action << "): "
                          << nf.text << std::endl;
            }
        }
    }
    return ok;
}

/* Apply one action to the current global state using gamma^- then gamma^+. */
static void applyAction(const std::string& action) {
    const auto minusIt = gammaMinus.find(action);
    if (minusIt != gammaMinus.end()) {
        const auto& tmpMinus = minusIt->second;
        for (const auto& entry : tmpMinus) {
            const std::string& prop = entry.first;
            formula* f = entry.second;
            assert(f != nullptr);
            const int val = f->evaluate();

            if (val == 1) {
                const auto lvIt = lv_key.find(prop);
                if (lvIt != lv_key.end()) {
                    const int key = lvIt->second;
                    const auto idxIt = lKey_index.find(key);
                    assert(idxIt != lKey_index.end());
                    lv_values[idxIt->second] = 0;
                } else {
                    const auto gvIt = gv_key.find(prop);
                    assert(gvIt != gv_key.end());
                    const int key = gvIt->second;
                    const auto idxIt = gKey_index.find(key);
                    assert(idxIt != gKey_index.end());
                    gv_values[idxIt->second] = 0;
                }
            }
        }
    }

    const auto plusIt = gammaPlus.find(action);
    if (plusIt != gammaPlus.end()) {
        const auto& tmpPlus = plusIt->second;
        for (const auto& entry : tmpPlus) {
            const std::string& prop = entry.first;
            formula* f = entry.second;
            assert(f != nullptr);
            const int val = f->evaluate();

            if (val == 1) {
                const auto lvIt = lv_key.find(prop);
                if (lvIt != lv_key.end()) {
                    const int key = lvIt->second;
                    const auto idxIt = lKey_index.find(key);
                    assert(idxIt != lKey_index.end());
                    lv_values[idxIt->second] = 1;
                } else {
                    const auto gvIt = gv_key.find(prop);
                    assert(gvIt != gv_key.end());
                    const int key = gvIt->second;
                    const auto idxIt = gKey_index.find(key);
                    assert(idxIt != gKey_index.end());
                    gv_values[idxIt->second] = 1;
                }
            }
        }
    }
}

/* Validate the provided action sequence against all values in Omega. */
static bool validatePlan(const std::vector<std::string>& plan) {
    if (plan.empty()) {
        for (NamedFormula& nf : local_formulas) {
            assert(nf.node != nullptr);
            nf.node->setLastAction();
        }
        for (NamedFormula& nf : global_formulas) {
            assert(nf.node != nullptr);
            nf.node->setLastAction();
        }
        return evaluateFormulas(local_formulas, "Local", 0, "") &&
               evaluateFormulas(global_formulas, "Global", 0, "");
    }

    if (!evaluateFormulas(local_formulas, "Local", 0, "") ||
        !evaluateFormulas(global_formulas, "Global", 0, "")) {
        return false;
    }

    for (int i = 0; i < static_cast<int>(plan.size()); i++) {
        const std::string& action = plan[i];
        const bool actionKnown =
            gammaMinus.find(action) != gammaMinus.end() ||
            gammaPlus.find(action) != gammaPlus.end();
        assert(actionKnown);

        if (i == static_cast<int>(plan.size()) - 1) {
            // FG is evaluated only on the final step, so mark it here.
            for (NamedFormula& nf : local_formulas) {
                assert(nf.node != nullptr);
                nf.node->setLastAction();
            }
            for (NamedFormula& nf : global_formulas) {
                assert(nf.node != nullptr);
                nf.node->setLastAction();
            }
        }

        applyAction(action);

        const int step = i + 1;
        // Enforce all values after every action and stop on first violation.
        const bool localsOk = evaluateFormulas(local_formulas, "Local", step, action);
        const bool globalsOk = evaluateFormulas(global_formulas, "Global", step, action);
        if (!localsOk || !globalsOk) {
            return false;
        }
    }
    return true;
}

int main(void){
 readProblem(std::cin);

 assert(lv_values != nullptr);

 const bool valid = validatePlan(actions);
 delete []lv_values;
 delete []gv_values;
 return valid ? 0 : -1;
}
