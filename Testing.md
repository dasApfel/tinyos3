# Test the Multilevel Feedback Queue :

  - Open 3 terminals (let's call them 1,2,3 for now)
  - cd parent folder of project to all of them
  - make project (see README.md for instructions) at terminal 3 (control terminal)
  - ./terminal 0 (at terminal 1)
  - ./terminal 1 (at terminal 2)
  - ./tinyos_shell 1 2 [one core- two terminals synced]
  - "symposium nphilos nbites" (terminal 1). [or "repeat nrep symposium nphilos nbites"] Ex. "repeat 10 symposium 100 10"
  - give simultaneous IO (terminal 2).
  - you shouldn't be seeing IO lagging (terminal 2),if you do something went wrong.



